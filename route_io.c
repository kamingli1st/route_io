#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <execinfo.h>

#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include "route_io.h"

#define RIO_DEBUG(msg) fprintf(stderr, "%s\n", msg)
#define RIO_ERROR(errmsg) fprintf(stderr, "%s - %s\n", errmsg, strerror(errno) )
#define RIO_MALLOC malloc
#define RIO_FREE(p) free(p);p=NULL
#define RIO_DEF_BUF_SIZE 1024
#define RIO_DEF_LOGGER_ stderr
#define RIO_IS_WRITABLE(ev) lfqueue_size(&ev->out_queue)
#define RIO_STRLEN(p) strlen((char*)p)

#define RIO_WAIT_FOR_READ_WRITE
#define RIO_RELEASE_WAIT_FOR_READ_WRITE

#define RIO_ADD_FD(instance, fd, ee) epoll_ctl(instance->epfd, EPOLL_CTL_ADD, fd, ee)
#define RIO_MODIFY_FD(instance, fd, ee) epoll_ctl(instance->epfd, EPOLL_CTL_MOD, fd, ee)
#define RIO_DEL_CLOSE_FD(instance, fd, udphevt, ee)\
ee->data.ptr = NULL;\
if(epoll_ctl(instance->epfd, EPOLL_CTL_DEL, fd, NULL) != -1) {\
rio_do_close(fd);\
if(udphevt)RIO_FREE(udphevt);}else RIO_ERROR("error while del fd")

#define RIO_FREE_REQ \
if(req){\
if(req->in_buff)RIO_FREE(req->in_buff);\
if(req->out_buff)RIO_FREE(req->out_buff);\
ev->on_conn_close_handler(req);\
RIO_FREE(req);}

#define RIO_TCP_CHECK_TRY(n, nextstep, rt) \
if(n<0){\
if (errno == EWOULDBLOCK || errno == EINTR) {\
nextstep;\
}else if (errno != EAGAIN){\
fprintf(stderr, "error while process socket read/write: %s\n",strerror(errno));\
rt;\
}else fprintf(stderr, "tcp:Error: %s\n", strerror(errno) );\
}else if(n == 0) { \
rt;}

typedef void (*rio_signal_handler_pt)(int);

static struct sigaction sa;
static int  has_init_signal = 0;
static int setnonblocking(int fd);
static int settimeout(int fd, int recv_timeout_ms, int send_timeout_ms);
/*** temporary disable for unused warning ***/
// static int setlinger(int sockfd, int onoff, int timeout_sec);
static int rio_do_close(int fd);
static void rio_add_signal_handler(rio_signal_handler_pt signal_handler);
static void rio_signal_backtrace(int sfd);
static int rio_run_epoll(rio_instance_t *instance);
static int rio_run_epoll_t(rio_instance_t *instance);

void *rio_read_handler_spawn(void *req_);
void *rio_read_tcp_handler_spawn(void *req_);

static int
rio_do_close(int fd) {
  int r;
  do {
    shutdown(fd, SHUT_RDWR);
    r = close(fd);
  } while (r == -1 && errno == EINTR);

  return r;
}

void
rio_write_output_buffer(rio_request_t *req, u_char* output) {
  size_t outsz = RIO_STRLEN(output);
  if (outsz == 0) {
    return ;
  }
  rio_buf_t *buf = RIO_MALLOC(sizeof(rio_buf_t) + outsz);
  if (!buf) {
    RIO_ERROR("malloc");
    return;
  }
  buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
  buf->end = ((u_char *)memcpy( buf->start, output, outsz)) + outsz ;
  req->out_buff = buf;

  if (req->event->isudp) {
    while (lfqueue_enq(&req->event->out_queue, req) != 1) { RIO_DEBUG("QUEUE INFINITE LOOP"); };
  } else {
    req->event->out_req = req;
  }
}

void
rio_write_output_buffer_l(rio_request_t *req, u_char* output, size_t len) {
  if (len == 0) {
    return ;
  }
  rio_buf_t *buf = RIO_MALLOC(sizeof(rio_buf_t) + len + 1);
  if (!buf) {
    RIO_ERROR("malloc");
    return;
  }
  buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
  buf->end = ((u_char *)memcpy( buf->start, output, len)) + len ;
  *buf->end++ = '\n' ;
  req->out_buff = buf;

  if (req->event->isudp) {
    while ( lfqueue_enq(&req->event->out_queue, req) != 1 ) { RIO_DEBUG("QUEUE INFINITE LOOP");};
  } else {
    req->event->out_req = req;
  }
}

static int
rio_create_fd(rio_event_t *ev, u_short port, short af_family, int socket_type, int protocol, int backlog) {
  struct sockaddr_in serveraddr;
  int sockfd;
  static int optval = 1;

  if ((sockfd = socket(af_family, socket_type/* SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC*/, protocol))
      == -1)
    return -1;

  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

  bzero((char *) &serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = af_family;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons(port);

  if (!ev->isudp) {

    if (setnonblocking(sockfd) == -1 /*|| setlinger(sockfd, 1, 3) == -1 || settimeout(sockfd, 1000, 1000) == -1 */) {
      RIO_ERROR("Error while creating fd");
      return -1;
    }

    struct epoll_event ee = { .data.ptr = (void*) ev, .events = EPOLLIN | EPOLLRDHUP | EPOLLERR };
    if (RIO_ADD_FD(ev->instance, sockfd, &ee )) {
      RIO_ERROR("error add_to_epoll_fd");
      return -1;
    }
  } else {
    struct epoll_event ee = { .data.ptr = (void*) ev, .events = EPOLLOUT | EPOLLIN | EPOLLRDHUP | EPOLLERR };
    if (RIO_ADD_FD(ev->instance, sockfd, &ee )) {
      RIO_ERROR("error add_to_epoll_fd");
      return -1;
    }
  }

  if (bind(sockfd, (struct sockaddr *) &serveraddr, sizeof serveraddr) == -1)
    return -1;

  if (!ev->isudp) {
    if (listen(sockfd, backlog) < 0) {
      fprintf(stderr, "%s\n", "could not open socket for listening\n");
      return -1;
    }
  }

  return sockfd;
}

void *
rio_read_handler_spawn(void *req_) {
  rio_request_t *req = req_;
  rio_event_t *ev = req->event;
  ev->read_handler(req);

  if ( RIO_IS_WRITABLE(ev) /*&& (events & EPOLLOUT)*/) {
    while ( (req = lfqueue_deq(&ev->out_queue)) ) {
      while (sendto(ev->sockfd, req->out_buff->start, req->out_buff->end - req->out_buff->start, 0,
                    (struct sockaddr *) &ev->client_addr, ev->client_addrlen) == -1 && errno == EINTR) /*Loop till success or error*/;

      RIO_FREE_REQ;
    }
  }
  pthread_exit(NULL);
}

void *
rio_read_tcp_handler_spawn(void *req_) {
  rio_request_t *req = req_;
  // rio_instance_t *instance = req->instance;
  rio_event_t *ev = req->event;
  int fd;
  rio_buf_t * buf;
  int bytes_read, bytes_send, est_bytes_left;

  if (req->sockfd < 0)
    goto ERROR_EXIT_REQUEST;

  fd = req->sockfd;
  buf = RIO_MALLOC(sizeof(rio_buf_t) + RIO_DEF_BUF_SIZE );
  if (buf == NULL) {
    RIO_ERROR("No Enough memory allocated");
    goto ERROR_EXIT_REQUEST;
  }
  buf->total_size = RIO_DEF_BUF_SIZE;
  buf->start = buf->end = ((u_char*) buf) + sizeof(rio_buf_t);
  rio_buf_t *new_buf;

REREAD:
  do {
    if ((bytes_read = recv( fd , buf->end, RIO_DEF_BUF_SIZE, 0)) > 0 ) {
      buf->end += bytes_read;
      size_t curr_size = buf->end - buf->start;
      if ( curr_size + RIO_DEF_BUF_SIZE >= buf->total_size ) {
        new_buf = RIO_MALLOC(sizeof(rio_buf_t) + buf->total_size * 2);
        if (!new_buf) {
          RIO_ERROR("Error creating thread\n");
          goto EXIT_REQUEST;
        }
        new_buf->start = ((u_char*) new_buf) + sizeof(rio_buf_t);
        new_buf->end = ((u_char*) memcpy(new_buf->start, buf->start, curr_size)) + curr_size;
        new_buf->total_size = buf->total_size * 2;
        RIO_FREE(buf);
        buf = new_buf;
      }
#if defined _WIN32 || _WIN64
      ioctlsocket(fd, FIONREAD, &est_bytes_left);
#else
      ioctl(fd, FIONREAD, &est_bytes_left);
#endif
    }
  } while (est_bytes_left > 0);

  RIO_TCP_CHECK_TRY(bytes_read, goto REREAD, goto EXIT_REQUEST);

  if ((buf->end - buf->start) == 0) {
    goto EXIT_REQUEST;
  }
  req->in_buff = buf;

  ev->read_handler(req);

  if ( req->out_buff && (bytes_send = req->out_buff->end - req->out_buff->start) ) {
    while ( (bytes_read = send(req->sockfd, req->out_buff->start, bytes_send, 0)) < 0) {
      RIO_TCP_CHECK_TRY(bytes_read, continue, goto EXIT_REQUEST);
    }
  }

  if (buf) {
    buf->start = buf->end;
  }
  if (req->out_buff) {
    RIO_FREE(req->out_buff);
    req->out_buff = NULL;
  }

  goto REREAD;

EXIT_REQUEST:
  RIO_FREE(buf);
ERROR_EXIT_REQUEST:
  if (req) {
    rio_do_close(req->sockfd);
    if (req->out_buff)RIO_FREE(req->out_buff);
    ev->on_conn_close_handler(req);
    RIO_FREE(req);
  }
  pthread_exit(NULL);
}

static int
rio_run_epoll(rio_instance_t *instance) {
  struct epoll_event *ep_events = instance->ep_events;
  struct epoll_event *epev;
  rio_event_t *ev;
  int i, n;
  int events;
  int fd;
  int bytes_read;
  rio_request_t *req;
  rio_buf_t *buf;

  memset(ep_events, 0, instance->ep_events_sz);
  do {
    n = epoll_wait(instance->epfd, ep_events, instance->nevents, 5000);
  } while (n == -1 && errno == EINTR);

  if (n == -1) {
    return -1;
  }

  for (i = 0; i < n; i++) {
    epev = &ep_events[i];
    events = epev->events;
    ev = epev->data.ptr;

    if (ev->isudp) {
      fd = ev->sockfd;
      /**UDP**/
      if ( events & EPOLLIN  ) {

#if defined _WIN32 || _WIN64
        ioctlsocket(fd, FIONREAD, &bytes_read);
#else
        ioctl(fd, FIONREAD, &bytes_read);
#endif
        if (bytes_read > 0) {
          buf = RIO_MALLOC(sizeof(rio_buf_t) + bytes_read);
          if (buf == NULL) {
            RIO_ERROR("No Enough memory allocated");
            return -1;
          }
          buf->start = ((u_char*) buf) + sizeof(rio_buf_t);

          while (recvfrom(fd, buf->start, bytes_read, 0,
                          (struct sockaddr *) &ev->client_addr, &ev->client_addrlen) == -1 && errno == EINTR) /*Loop till success or error*/;

          buf->end = buf->start + bytes_read;
          // ev->in_buff = buf;
          req = RIO_MALLOC(sizeof(rio_request_t));
          if (req == NULL) {
            RIO_ERROR("No Enough memory allocated");
            return -1;
          }
          req->sockfd = fd;
          req->in_buff = buf;
          req->out_buff = NULL;
          req->event = ev;
          req->ctx_val = NULL;

          rio_read_handler_spawn(req);
        }
      }
    } else {
      if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        RIO_DEL_CLOSE_FD(ev->instance, ev->sockfd, ev, epev);
        continue;
      }
      // Get new connection
      if ((fd = accept( ev->sockfd, (struct sockaddr *)&ev->client_addr,
                        &ev->client_addrlen)) < 0) {
        RIO_ERROR("Error while accepting port\n");
        continue;
      }

      if ( settimeout(fd, 1000, 1000) == -1 ) {
        return -1;
      }

      req = RIO_MALLOC(sizeof(rio_request_t));
      if (req == NULL) {
        RIO_ERROR("No Enough memory allocated");
        return ENOMEM;
      }

      req->sockfd = fd;
      req->event = ev;
      req->out_buff = NULL;
      req->ctx_val = NULL;
      rio_read_tcp_handler_spawn(req);
    }
  }
  return 0;
}

static int
rio_run_epoll_t(rio_instance_t *instance) {
  struct epoll_event *ep_events = instance->ep_events;
  struct epoll_event *epev;
  rio_event_t *ev;
  int i, n;
  int events;
  int fd;
  int bytes_read;
  rio_request_t *req;
  rio_buf_t *buf;

  memset(ep_events, 0, instance->ep_events_sz);
  do {
    n = epoll_wait(instance->epfd, ep_events, instance->nevents, 5000);
  } while (n == -1 && errno == EINTR);

  if (n == -1) {
    return -1;
  }

  for (i = 0; i < n; i++) {
    epev = &ep_events[i];
    events = epev->events;
    ev = epev->data.ptr;

    if (ev->isudp) {
      fd = ev->sockfd;
      /**UDP**/
      if ( events & EPOLLIN  ) {

#if defined _WIN32 || _WIN64
        ioctlsocket(fd, FIONREAD, &bytes_read);
#else
        ioctl(fd, FIONREAD, &bytes_read);
#endif
        if (bytes_read > 0) {
          buf = RIO_MALLOC(sizeof(rio_buf_t) + bytes_read);
          if (buf == NULL) {
            RIO_ERROR("No Enough memory allocated");
            return -1;
          }
          buf->start = ((u_char*) buf) + sizeof(rio_buf_t);

          while (recvfrom(fd, buf->start, bytes_read, 0,
                          (struct sockaddr *) &ev->client_addr, &ev->client_addrlen) == -1 && errno == EINTR) /*Loop till success or error*/;

          buf->end = buf->start + bytes_read;
          // ev->in_buff = buf;
          req = RIO_MALLOC(sizeof(rio_request_t));
          if (req == NULL) {
            RIO_ERROR("No Enough memory allocated");
            return -1;
          }
          req->sockfd = fd;
          req->in_buff = buf;
          req->out_buff = NULL;
          req->event = ev;
          req->ctx_val = NULL;
          // req->instance = ev->instance;
          pthread_t t;
          if (pthread_create(&t, NULL, rio_read_handler_spawn, req)) {
            RIO_FREE(req->in_buff);
            RIO_FREE(req);
            RIO_ERROR("Error creating thread\n");
            return -1;
          }
          pthread_detach(t);
        }
      }
    } else {
      if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        RIO_DEL_CLOSE_FD(ev->instance, ev->sockfd, ev, epev);
        continue;
      }
      // Get new connection
      if ((fd = accept( ev->sockfd, (struct sockaddr *)&ev->client_addr,
                        &ev->client_addrlen)) < 0) {
        RIO_ERROR("Error while accepting port\n");
        continue;
      }

      if ( settimeout(fd, 1000, 1000) == -1 ) {
        return -1;
      }

      req = RIO_MALLOC(sizeof(rio_request_t));
      if (req == NULL) {
        RIO_ERROR("No Enough memory allocated");
        return ENOMEM;
      }

      req->sockfd = fd;
      req->event = ev;
      req->out_buff = NULL;
      req->ctx_val = NULL;
      // req->instance = ev->instance;

      pthread_t t;
      if (pthread_create(&t, NULL, rio_read_tcp_handler_spawn, req)) {
        RIO_FREE(req);
        RIO_ERROR("Error creating thread\n");
        return -1;
      }
      pthread_detach(t);
    }
  }
  return 0;
}

#define any_child_pid -1

int
rio_start(rio_instance_t *instance, int with_threads) {
  int r, child_status, i, max_queue_sz;
  pid_t ch_pid;

STREAM_RESTART:
  if (!has_init_signal) {
    rio_add_signal_handler(rio_signal_backtrace);
  }
  ch_pid = fork();
  if (ch_pid == -1) {
    perror("fork");
    exit(EXIT_FAILURE);
  }

  if (ch_pid == 0) {

    /** Init udp lfqueue on child process **/
    for (i = 0; i < instance->n; i++) {
      rio_event_t * ev = &instance->evts[i];
      if (ev->isudp) {
        max_queue_sz = ev->max_message_queue;
        lfqueue_init(&ev->out_queue, max_queue_sz);
      }
    }


    if (instance->init_handler) {
      instance->init_handler(instance->init_arg);
    }

    if (with_threads) {
      while ((r = rio_run_epoll_t(instance)) == 0) /*loop*/;
    } else {
      while ((r = rio_run_epoll(instance)) == 0) /*loop*/;
    }
    if (r == -1) {
      RIO_ERROR("error while processing ");
      exit(EXIT_SUCCESS);
    }
  } else {
    while (1) {
      if (waitpid(ch_pid /*any_child_pid*/, &child_status, WNOHANG) == ch_pid) {
        has_init_signal = 0;
        goto STREAM_RESTART;
      }
      sleep(1);
    }
  }
  return 0;
}

rio_instance_t*
rio_create_routing_instance(int max_service_port, rio_init_handler_pt init_handler, void* arg) {
  int i;
  rio_instance_t *instance;
  rio_event_t *events;
  struct epoll_event *ep_events;
  instance = RIO_MALLOC(sizeof(rio_instance_t));

  if (!instance) {
    RIO_ERROR("malloc");
    return NULL;
  }

  if ((instance->epfd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
    fprintf(stderr, "%s\n", "error create epoll");
    return NULL;
  }

  if ((events = RIO_MALLOC(max_service_port * sizeof(rio_event_t))) == NULL) {
    fprintf(stderr, "%s\n", "error malloc");
    return NULL;
  }


  if ((ep_events = RIO_MALLOC(max_service_port * sizeof(struct epoll_event))) == NULL) {
    fprintf(stderr, "%s\n", "error malloc");
    return NULL;
  }

  for (i = 0; i < max_service_port; i++) {
    events[i].out_req = NULL;
    events[i].instance = instance;
    events[i].client_addrlen = sizeof(events[i].client_addr);
    events[i].read_handler = NULL;
    bzero((char *) &events[i].client_addr, events[i].client_addrlen);
  }

  instance->evts = events;
  instance->ep_events = ep_events;
  instance->ep_events_sz = max_service_port * sizeof(struct epoll_event);
  instance->nevents = max_service_port;
  instance->n = 0; /*Default*/
  instance->init_handler = init_handler;
  instance->init_arg = arg;
  // instance->read_handler = read_handler;
  // instance->signal_handler = signal_handler;


  return instance;
}

static int
setnonblocking(int fd) {
#if defined _WIN32 || _WIN64
  unsigned long nonblock = 1;
  return (ioctlsocket(fd, FIONBIO, &nonblock) == 0) ? true : false;
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    RIO_ERROR("error while configure fd non blocking");
    return -1;
  }
  flags = (flags | O_NONBLOCK);
  if (fcntl(fd, F_SETFL, flags) != 0) {
    RIO_ERROR("error while configure fd non blocking");
    return -1;
  }
#endif
  return 0;
}

/*** temporary disable for unused warning ***/
// static int
// setlinger(int sockfd, int onoff, int timeout_sec) {
//   struct linger l;
//   l.l_onoff  = onoff;
//   l.l_linger = timeout_sec;
//   if (setsockopt(sockfd, SOL_SOCKET, SO_LINGER, (char *) &l, sizeof(l)) < 0) {
//     return RIO_ERROR("Error while setting linger");
//   };
// }

static int
settimeout(int fd, int recv_timeout_ms, int send_timeout_ms) {
  struct timeval send_tmout_val;
  struct timeval recv_tmout_val;


  recv_tmout_val.tv_sec = (recv_timeout_ms >= 1000) ?  recv_timeout_ms / 1000 : 0; // Default 1 sec time out
  recv_tmout_val.tv_usec = (recv_timeout_ms % 1000) * 1000 ;
  if (setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &recv_tmout_val,
                  sizeof(recv_tmout_val)) < 0) {
    RIO_ERROR("setsockopt recv_tmout_val failed\n");
    return -1;
  }

  send_tmout_val.tv_sec = (send_timeout_ms >= 1000) ? send_timeout_ms / 1000 : 0; // Default 1 sec time out
  send_tmout_val.tv_usec = (send_timeout_ms % 1000) * 1000 ;
  if (setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &send_tmout_val,
                  sizeof(send_tmout_val)) < 0) {
    RIO_ERROR("setsockopt send_tmout_val failed\n");
    return -1;
  }

  return 0;
}

static void
rio_add_signal_handler(rio_signal_handler_pt signal_handler) {
  memset(&sa, 0, sizeof(struct sigaction));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);

  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGIOT, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);
#ifdef SIGBUS
  sigaction(SIGBUS, &sa, NULL);
#endif
  has_init_signal = 1;
}

static void
rio_signal_backtrace(int sfd) {
  size_t i, ptr_size;
  void *buffer[10];
  char **strings;

  ptr_size = backtrace(buffer, 1024);
  fprintf(stderr, "backtrace() returned %zd addresses\n", ptr_size);

  strings = backtrace_symbols(buffer, ptr_size);
  if (strings == NULL) {
    RIO_ERROR("backtrace_symbols");
    exit(EXIT_FAILURE);
  }

  for (i = 0; i < ptr_size; i++)
    fprintf(stderr, "%s\n", strings[i]);

  free(strings);
  exit(EXIT_FAILURE);
}

static void rio_def_On_conn_close_handler(rio_request_t *req) {
  /*Do nothing*/
}

int
rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int max_message_queue, rio_on_conn_close_pt on_conn_close_handler) {
  if (read_handler == NULL) {
    RIO_ERROR("Read handler cannot be NULL");
    return -1;
  }

  if (!instance || instance->nevents == 0 ||  instance->nevents <= instance->n) {
    RIO_ERROR("error while adding service port");
    return -1;
  }

  rio_event_t * ev = &instance->evts[instance->n++];

  ev->read_handler = read_handler;
  if (on_conn_close_handler == NULL) {
    on_conn_close_handler = rio_def_On_conn_close_handler;
  }
  ev->on_conn_close_handler = on_conn_close_handler;
  ev->max_message_queue = max_message_queue;

  ev->isudp = 1;
  ev->is_listener = 1;

  if ((ev->sockfd = rio_create_fd(ev, port, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, 0 )) == -1) {
    RIO_ERROR("error rio_create_fd");
    return -1;
  }

  return 0;
}

int
rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog, rio_on_conn_close_pt on_conn_close_handler) {
  if (read_handler == NULL) {
    RIO_ERROR("Read handler cannot be NULL");
    return -1;
  }

  if (!instance || instance->nevents == 0 ||  instance->nevents <= instance->n) {
    RIO_ERROR("error while adding service port");
    return -1;
  }

  rio_event_t * ev = &instance->evts[instance->n++];
  ev->read_handler = read_handler;
  if (on_conn_close_handler == NULL) {
    on_conn_close_handler = rio_def_On_conn_close_handler;
  }
  ev->on_conn_close_handler = on_conn_close_handler;
  ev->isudp = 0;
  ev->is_listener = 1;

  if ((ev->sockfd = rio_create_fd(ev, port, AF_INET, SOCK_STREAM, 0, backlog )) == -1) {
    RIO_ERROR("error rio_sockfd");
    return -1;
  }

  return 0;
}