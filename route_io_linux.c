#if !defined(__APPLE__) && !defined(_WIN32) && !defined(_WIN64)/*Linux*/

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
#include <fcntl.h>
#include "route_io.h"

#define RIO_DEBUG(msg) fprintf(stderr, "%s\n", msg)
#define RIO_ERROR(errmsg) fprintf(stderr, "%s - %s\n", errmsg, strerror(errno) )
#define RIO_BUFFER_SIZE 1024
#define RIO_MALLOC malloc
#define RIO_FREE(p) free(p);p=NULL
#define RIO_DEF_LOGGER_ stderr
// #define RIO_IS_WRITABLE(ev) lfqueue_size(&ev->out_queue)
#define RIO_STRLEN(p) strlen((char*)p)

#define RIO_ADD_FD(instance, fd, ee) epoll_ctl(instance->epfd, EPOLL_CTL_ADD, fd, ee)
#define RIO_MODIFY_FD(instance, fd, ee) epoll_ctl(instance->epfd, EPOLL_CTL_MOD, fd, ee)
#define RIO_DEL_FD(instance, fd, ee) epoll_ctl(instance->epfd, EPOLL_CTL_DEL, fd, ee)

#if EAGAIN == EWOULDBLOCK
#define RIO_EAGAIN_EBLOCK EAGAIN
#else
#define RIO_EAGAIN_EBLOCK EAGAIN: case EWOULDBLOCK
#endif

// This only use when it n <= 0
#define RIO_SOCKET_CHECK_TRY(n, goto_retry, goto_close) \
if(n==0){ \
goto_close;\
}else{\
switch(errno){\
case RIO_EAGAIN_EBLOCK:\
case EINTR:\
goto_retry;\
default:\
fprintf(stderr, "socket:Error: %s\n", strerror(errno) );\
goto_close; \
}}

#define RIO_FREE_REQ_ALL(req) \
if(req){\
if (req->inbuf) { \
RIO_FREE(req->inbuf); \
} \
if (req->udp_outbuf) { \
RIO_FREE(req->udp_outbuf); \
} \
RIO_FREE(req);}

typedef void (*rio_signal_handler_pt)(int);

static int __RIO_MAX_POLLING_EVENT__ = 128;
static int __RIO_NO_FORK_PROCESS__ = 0; // By default it fork a process
static int __RIO_DEF_SZ_PER_READ__ = RIO_BUFFER_SIZE; // By default size, adjustable
static int __RIO_READ_TIMEOUT_MS__ = 0; // By default size, adjustable
static int __RIO_WRITE_TIMEOUT_MS__ = 0; // By default size, adjustable
static struct sigaction sa;
static int  has_init_signal = 0;
static int setnonblocking(int fd);
static int set_read_timeout(int fd, int read_timeout_ms);
static int set_write_timeout(int fd, int write_timeout_ms);
/*** temporary disable for unused warning ***/
// static int setlinger(int sockfd, int onoff, int timeout_sec);
static int rio_do_close(int fd);
static rio_buf_t* rio_realloc_buf(rio_buf_t *);
static void rio_add_signal_handler(rio_signal_handler_pt signal_handler);
static void rio_signal_backtrace(int sfd);
static int rio_run_epoll(rio_instance_t *instance);
static void rio_def_on_conn_close_handler(rio_request_t *req) {
	/*Do nothing*/
}
static short rio_get_port_from_socket(int sock) {
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	if (getsockname(sock, (struct sockaddr *)&sin, &len) == -1) {
		RIO_ERROR("getsockname");
	} else {
		return ntohs(sin.sin_port);
	}
	return -1;
}

void rio_read_udp_handler_queue(void *req_);
void rio_read_tcp_handler_queue(void *req_);

void
rio_set_no_fork() {
	__RIO_NO_FORK_PROCESS__ = 1;
}

void
rio_set_max_polling_event(int opt) {
	__RIO_MAX_POLLING_EVENT__ = opt;
}

void
rio_set_def_sz_per_read(int opt) {
	__RIO_DEF_SZ_PER_READ__ = opt;
}

void
rio_set_rw_timeout(int read_time_ms, int write_time_ms) {
	__RIO_READ_TIMEOUT_MS__ = read_time_ms;
	__RIO_WRITE_TIMEOUT_MS__ = write_time_ms;
}

void
rio_set_curr_req_read_sz(rio_request_t *req, int opt) {
	req->sz_per_read = opt;
}

rio_state
rio_write_output_buffer(rio_request_t *req, unsigned char* output) {
	int retbytes;
	size_t outsz, curr_size, new_size;
	rio_buf_t *buf;
	if (output) {
		outsz = RIO_STRLEN(output);
		if (outsz == 0) {
			return rio_ERROR;
		}
		if (req->isudp) {
			if (req->udp_outbuf == NULL) {
				buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + outsz);
				if (!buf) {
					RIO_ERROR("malloc");
					return rio_ERROR;
				}
				buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
				buf->end = ((u_char *)memcpy(buf->start, output, outsz)) + outsz;
				buf->capacity = outsz;
				req->udp_outbuf = buf;
			}
			else {
				curr_size = rio_buf_size(req->udp_outbuf);
				if ((curr_size + outsz) > req->udp_outbuf->capacity) {
					new_size = (curr_size + outsz) * 2;
					buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + new_size);
					if (!buf) {
						RIO_ERROR("malloc");
						return rio_ERROR;
					}
					buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
					buf->end = ((u_char*)memcpy(buf->start, req->udp_outbuf->start, curr_size)) + curr_size;
					buf->end = ((u_char*)memcpy(buf->end, output, outsz)) + outsz;
					buf->capacity = new_size;
					RIO_FREE(req->udp_outbuf);
					req->udp_outbuf = buf;
				}
				else {
					buf = req->udp_outbuf;
					buf->end = ((u_char*)memcpy(buf->end, output, outsz)) + outsz;
				}
			}
		} else {
			while ( ( retbytes = sendto(req->sockfd, output, outsz, 0,
			                            (struct sockaddr *) &req->client_addr, req->client_addr_len) ) <= 0 ) {
				RIO_SOCKET_CHECK_TRY(retbytes, printf("%s\n", "timeout while sending"); return rio_SOCK_TIMEOUT, printf("%s\n", "peer closed while sending"); return rio_ERROR);
			}
		}
	}
	return rio_SUCCESS;
}

rio_state
rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t outsz) {
	int retbytes;
	size_t curr_size, new_size;
	rio_buf_t *buf;
	if (output) {
		if (outsz == 0) {
			return rio_ERROR;
		}
		if (req->isudp) {
			if (req->udp_outbuf == NULL) {
				buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + outsz);
				if (!buf) {
					RIO_ERROR("malloc");
					return rio_ERROR;
				}
				buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
				buf->end = ((u_char *)memcpy(buf->start, output, outsz)) + outsz;
				buf->capacity = outsz;
				req->udp_outbuf = buf;
			}
			else {
				curr_size = rio_buf_size(req->udp_outbuf);
				if ((curr_size + outsz) > req->udp_outbuf->capacity) {
					new_size = (curr_size + outsz) * 2;
					buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + new_size);
					if (!buf) {
						RIO_ERROR("malloc");
						return rio_ERROR;
					}
					buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
					buf->end = ((u_char*)memcpy(buf->start, req->udp_outbuf->start, curr_size)) + curr_size;
					buf->end = ((u_char*)memcpy(buf->end, output, outsz)) + outsz;
					buf->capacity = new_size;
					RIO_FREE(req->udp_outbuf);
					req->udp_outbuf = buf;
				}
				else {
					buf = req->udp_outbuf;
					buf->end = ((u_char*)memcpy(buf->end, output, outsz)) + outsz;
				}
			}
		} else {
			while ( ( retbytes = sendto(req->sockfd, output, outsz, 0,
			                            (struct sockaddr *) &req->client_addr, req->client_addr_len) ) <= 0 ) {
				RIO_SOCKET_CHECK_TRY(retbytes, printf("%s\n", "timeout while sending"); return rio_SOCK_TIMEOUT, printf("%s\n", "peer closed while sending"); return rio_ERROR);
			}
		}
	}
	return rio_SUCCESS;
}

static int
rio_create_fd(u_short port, short af_family, int socket_type, int protocol, int backlog, int isudp) {
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

	if (setnonblocking(sockfd) == -1 /*|| setlinger(sockfd, 1, 3) == -1 */) {
		RIO_ERROR("Error while creating fd");
		return -1;
	}

	if (__RIO_READ_TIMEOUT_MS__ && set_read_timeout(sockfd, __RIO_READ_TIMEOUT_MS__) == -1) {
		RIO_ERROR("Error while creating fd");
		return -1;
	}

	if (__RIO_WRITE_TIMEOUT_MS__ && set_write_timeout(sockfd, __RIO_WRITE_TIMEOUT_MS__) == -1) {
		RIO_ERROR("Error while creating fd");
		return -1;
	}

	if (bind(sockfd, (struct sockaddr *) &serveraddr, sizeof serveraddr) == -1)
		return -1;

	if (!isudp) {
		if (listen(sockfd, backlog) < 0) {
			fprintf(stderr, "%s\n", "could not open socket for listening\n");
			return -1;
		}
	}
	return sockfd;
}

void
rio_read_udp_handler_queue(void *_req) {
	rio_request_t *req = (rio_request_t*)_req;
	int retbytes, fd = req->sockfd;
	rio_buf_t *inbuf = req->inbuf;
	size_t sz_per_read = req->sz_per_read, output_sz;
	while ( ( retbytes = recvfrom(fd, inbuf->start, sz_per_read, 0,
	                              (struct sockaddr *) &req->client_addr, &req->client_addr_len) ) <= 0 ) {
		RIO_SOCKET_CHECK_TRY(retbytes, goto READ_HANDLER, goto EXIT_REQUEST);
	}
	inbuf->end = inbuf->start + retbytes;
	req->inbuf = inbuf;
READ_HANDLER:
	req->read_handler(req);
	if (req->udp_outbuf && (output_sz = rio_buf_size(req->udp_outbuf))) {
		while ( ( retbytes = sendto(req->sockfd, req->udp_outbuf->start, output_sz, 0,
		                            (struct sockaddr *) &req->client_addr, req->client_addr_len) ) <= 0 ) {
			RIO_SOCKET_CHECK_TRY(retbytes, printf("%s\n", "timeout while sending"); goto EXIT_REQUEST, printf("%s\n", "peer closed while sending"); goto EXIT_REQUEST);
		}
	}
EXIT_REQUEST:
	RIO_FREE_REQ_ALL(req);
}

void
rio_read_tcp_handler_queue(void *req_) {
	int retbytes;
	rio_request_t *req = (rio_request_t*)req_;
	rio_buf_t *inbuf = req->inbuf;
	size_t curr_size, sz_per_read = req->sz_per_read, new_size;

	for (;;) {
		while ((retbytes = recv( req->sockfd , inbuf->end, sz_per_read, 0)) <= 0 ) {
			RIO_SOCKET_CHECK_TRY(retbytes, goto READ_HANDLER, goto EXIT_REQUEST);
		}
		inbuf->end += retbytes;
READ_HANDLER:
		req->read_handler(req);

		if (req->force_close) {
			goto EXIT_REQUEST;
		} else if (inbuf) {
			curr_size = rio_buf_size(inbuf);
			// It is dynamic change sz per read
			sz_per_read = req->sz_per_read;
			if ((inbuf->capacity - curr_size) < sz_per_read) {
				new_size = inbuf->capacity * 2;
				while ((new_size - curr_size) < sz_per_read) {
					new_size *= 2;
				}
				inbuf->capacity = new_size;
				inbuf = req->inbuf = rio_realloc_buf(inbuf);
			}
		}
	}
	/** Exception Case, it might not be happened ***/
EXIT_REQUEST:
	if (req) {
		req->on_conn_close_handler(req);
		rio_do_close(req->sockfd);
		RIO_FREE_REQ_ALL(req);
	}
}

static int
rio_do_close(int fd) {
	int r;
	do {
		shutdown(fd, SHUT_RDWR);
		r = close(fd);
	} while (r == -1 && errno == EINTR);

	return r;
}

static rio_buf_t*
rio_realloc_buf(rio_buf_t * buf) {
	size_t curr_size = rio_buf_size(buf);
	rio_buf_t*	new_buf  = RIO_MALLOC(sizeof(rio_buf_t) + buf->capacity);
	if (new_buf == NULL) {
		RIO_ERROR("malloc ");
		return NULL;
	}
	new_buf->capacity = buf->capacity;
	new_buf->start = ((u_char*) new_buf) + sizeof(rio_buf_t);
	new_buf->end = ((u_char*) memcpy(new_buf->start, buf->start, curr_size)) + curr_size;
	RIO_FREE(buf);
	return new_buf;
}

static int
rio_run_epoll(rio_instance_t *instance) {
	struct epoll_event *ep_events = instance->ep_events;
	struct epoll_event *epev;
	rio_request_t *main_req, *sub_req;
	int i, n, evstate,
	    fd;
	size_t sz_per_read;
	rio_buf_t *buf;

	memset(ep_events, 0, instance->ep_events_sz);
	do {
		n = epoll_wait(instance->epfd, ep_events, __RIO_MAX_POLLING_EVENT__, 5000);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		return -1;
	}

	// printf("%d\n", n);

	for (i = 0; i < n; i++) {
		epev = &ep_events[i];
		evstate = epev->events;
		main_req = epev->data.ptr;

		if ( evstate & EPOLLIN  ) {
			if (main_req->isudp) {
				fd = main_req->sockfd;
				sz_per_read = __RIO_DEF_SZ_PER_READ__;
				buf = (rio_buf_t*) RIO_MALLOC(sizeof(rio_buf_t) + sz_per_read);
				if (buf == NULL) {
					RIO_ERROR("No Enough memory allocated");
					continue;
				}
				buf->end = buf->start = ((u_char*) buf) + sizeof(rio_buf_t);
				buf->capacity = sz_per_read;

				sub_req = (rio_request_t*)RIO_MALLOC(sizeof(rio_request_t));
				memcpy(sub_req, main_req, sizeof(rio_request_t));
				sub_req->sockfd = fd;//fcntl(fd, F_DUPFD, 0);
				sub_req->sz_per_read = sz_per_read;
				sub_req->inbuf = buf;
				if (at_thpool_newtask(instance->thpool, rio_read_udp_handler_queue, sub_req) < 0) {
					RIO_ERROR("Too many job load, please expand the thread pool size");
					RIO_FREE(sub_req->inbuf);
					RIO_FREE(sub_req);
				}
			} else {
				// if inbuf is null, means is master request, accept new connection
				if ( main_req->inbuf == NULL) {
					sz_per_read = __RIO_DEF_SZ_PER_READ__;
					if ((fd = accept( main_req->sockfd, (struct sockaddr *)&main_req->client_addr,
					                  &main_req->client_addr_len)) < 0) {
						RIO_ERROR("Error while accepting port\n");
						continue;
					}

					// if ( settimeout(fd, 1000, 1000) == -1 ) {
					// 	continue;
					// }
					if (instance->nevents < __RIO_MAX_POLLING_EVENT__) {
						sub_req = RIO_MALLOC(sizeof(rio_request_t));
						if (sub_req == NULL) {
							RIO_ERROR("No Enough memory allocated");
							return ENOMEM;
						}
						memcpy(sub_req, main_req, sizeof(rio_request_t));
						sub_req->sockfd = fd;
						sub_req->sz_per_read = sz_per_read;
						sub_req->inbuf = buf = RIO_MALLOC(sizeof(rio_buf_t) + sz_per_read );
						if (buf == NULL) {
							RIO_ERROR("No Enough memory allocated");
							return ENOMEM;
						}
						buf->capacity = sz_per_read;
						buf->start = buf->end = ((u_char*) buf) + sizeof(rio_buf_t);
						struct epoll_event ee = { .data.ptr = (void*) sub_req, .events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP | EPOLLERR };
						if (RIO_ADD_FD(instance, sub_req->sockfd, &ee )) {
							RIO_ERROR("error add_to_epoll_fd");
							continue;
						}
						instance->nevents++;
					} else {
						fprintf(stderr, "%s\n", "Route has reached max events pool");
					}

				} else {
					/***remove event from EPOLL ***/
					if (RIO_DEL_FD(instance, main_req->sockfd, epev )) {
						RIO_ERROR("error delete_to_epoll_fd");
					}
					instance->nevents--;
					if (at_thpool_newtask(instance->thpool, rio_read_tcp_handler_queue, main_req) < 0) {
						fprintf(stderr, "%s\n", "Error Running on thread pool");
						rio_do_close(main_req->sockfd);
						RIO_FREE_REQ_ALL(main_req);
					}
					continue;
				}
			}
		} else if (evstate & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
			if (main_req->isudp) {
				fprintf(stderr, "Error/closed on udp port %d\n", rio_get_port_from_socket(main_req->sockfd));
				main_req->on_conn_close_handler(main_req);
			} else if (main_req->inbuf == NULL) { // only show the tcp listening port
				fprintf(stderr, "Error on tcp port %d\n", rio_get_port_from_socket(main_req->sockfd));
			}
			goto RIO_DEL_AND_FREE_EVENT;
		} else {
			// Keep polling
		}

		continue;

RIO_DEL_AND_FREE_EVENT:
		if (RIO_DEL_FD(instance, main_req->sockfd, epev )) {
			RIO_ERROR("error delete_to_epoll_fd");
		}
		instance->nevents--;
		rio_do_close(main_req->sockfd);
		RIO_FREE_REQ_ALL(main_req);
	}
	return 0;
}

#define any_child_pid -1

int
rio_start(rio_instance_t *instance, unsigned int nthreads) {
	int r, child_status;
	pid_t ch_pid;
STREAM_RESTART:
	if (!has_init_signal) {
		rio_add_signal_handler(rio_signal_backtrace);
	}
	if (__RIO_NO_FORK_PROCESS__) {
		ch_pid = 0;
	} else {
		ch_pid = fork();
	}
	if (ch_pid == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if (ch_pid == 0) {
		/** Init epoll events **/
		instance->ep_events_sz = __RIO_MAX_POLLING_EVENT__ * sizeof(struct epoll_event);
		if ((instance->ep_events = (struct epoll_event*) RIO_MALLOC(instance->ep_events_sz)) == NULL) {
			fprintf(stderr, "%s\n", "error malloc");
			sleep(2);
			return -1;
		}

		instance->thpool = at_thpool_create(nthreads);

		if (instance->init_handler) {
			instance->init_handler(instance->init_arg);
		}

		while ((r = rio_run_epoll(instance)) == 0) /*loop*/;

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
rio_create_routing_instance(rio_init_handler_pt init_handler, void* arg) {
	rio_instance_t *instance;
	instance = RIO_MALLOC(sizeof(rio_instance_t));

	if (!instance) {
		RIO_ERROR("malloc");
		return NULL;
	}

	if ((instance->epfd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
		fprintf(stderr, "%s\n", "error create epoll");
		return NULL;
	}

	instance->nevents = 0;
	instance->init_handler = init_handler;
	instance->init_arg = arg;

	return instance;
}

static int
setnonblocking(int fd) {
	// Non blocking is not needed as it is multhreading
	/*#if defined _WIN32 || _WIN64
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
	#endif*/
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
set_read_timeout(int fd, int recv_timeout_ms) {
	struct timeval recv_tmout_val;
	recv_tmout_val.tv_sec = (recv_timeout_ms >= 1000) ?  recv_timeout_ms / 1000 : 0; // Default 1 sec time out
	recv_tmout_val.tv_usec = (recv_timeout_ms % 1000) * 1000 ;
	if (setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &recv_tmout_val,
	                sizeof(recv_tmout_val)) < 0) {
		RIO_ERROR("setsockopt recv_tmout_val failed\n");
		return -1;
	}

	return 0;
}

static int
set_write_timeout(int fd, int write_timeout_ms) {
	struct timeval send_tmout_val;

	send_tmout_val.tv_sec = (write_timeout_ms >= 1000) ? write_timeout_ms / 1000 : 0; // Default 1 sec time out
	send_tmout_val.tv_usec = (write_timeout_ms % 1000) * 1000 ;
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

int
rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, rio_on_conn_close_pt on_conn_close_handler) {
	int fd;
	const int defSize = __RIO_DEF_SZ_PER_READ__;
	rio_request_t *p_req;
	if (read_handler == NULL) {
		RIO_ERROR("Read handler cannot be NULL");
		return -1;
	}

	if (instance == NULL) {
		RIO_ERROR("error while adding service port");
		return -1;
	}

	if ((fd = rio_create_fd(port, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, 0/*no backlog*/, 1/*udp*/ )) == -1) {
		RIO_ERROR("error rio_create_fd");
		return -1;
	}

	// udp only one fd event
	p_req = (rio_request_t*)RIO_MALLOC(sizeof(rio_request_t));
	p_req->read_handler = read_handler;
	if (on_conn_close_handler == NULL) {
		on_conn_close_handler = rio_def_on_conn_close_handler;
	}
	p_req->on_conn_close_handler = on_conn_close_handler;
	p_req->isudp = 1;
	p_req->udp_outbuf = NULL;
	p_req->force_close = 0;
	p_req->sz_per_read = defSize;
	p_req->sockfd = fd;//fcntl(fd, F_DUPFD, 0);

	/**Master FD has no request needed**/
	p_req->inbuf = NULL;
	p_req->ctx_val = NULL;
	p_req->client_addr_len = sizeof(p_req->client_addr);
	bzero((char *) &p_req->client_addr, sizeof(p_req->client_addr));

	struct epoll_event ee = { .data.ptr = (void*) p_req, .events = EPOLLIN | EPOLLRDHUP | EPOLLERR };
	if (RIO_ADD_FD(instance, p_req->sockfd, &ee )) {
		RIO_ERROR("error add_to_epoll_fd");
		return -1;
	}
	instance->nevents += 1;
	return 0;
}

int
rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog, rio_on_conn_close_pt on_conn_close_handler) {
	int fd;
	const int defSize = __RIO_DEF_SZ_PER_READ__;

	rio_request_t *p_req;
	if (read_handler == NULL) {
		RIO_ERROR("Read handler cannot be NULL");
		return -1;
	}

	if (instance == NULL) {
		RIO_ERROR("error while adding service port");
		return -1;
	}

	if ((fd = rio_create_fd(port, AF_INET, SOCK_STREAM, 0, backlog, 0 )) == -1) {
		RIO_ERROR("error rio_sockfd");
		return -1;
	}

	p_req = (rio_request_t*)RIO_MALLOC(sizeof(rio_request_t));
	p_req->read_handler = read_handler;
	if (on_conn_close_handler == NULL) {
		on_conn_close_handler = rio_def_on_conn_close_handler;
	}
	p_req->on_conn_close_handler = on_conn_close_handler;
	p_req->isudp = 0;
	p_req->udp_outbuf = NULL;
	p_req->force_close = 0;
	p_req->sz_per_read = defSize;
	p_req->sockfd = fd;

	/**Master FD has no request needed**/
	p_req->inbuf = NULL;
	p_req->ctx_val = NULL;
	p_req->client_addr_len = sizeof(p_req->client_addr);
	bzero((char *) &p_req->client_addr, sizeof(p_req->client_addr));

	struct epoll_event ee = { .data.ptr = (void*) p_req, .events = EPOLLIN | EPOLLRDHUP | EPOLLERR };
	if (RIO_ADD_FD(instance, fd, &ee )) {
		RIO_ERROR("error add_to_epoll_fd");
		return -1;
	}
	/*For waiting the extra accepted fd events*/
	instance->nevents += 1;
	return 0;
}

#endif
