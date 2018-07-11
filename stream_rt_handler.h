#ifndef STREAM_RT_HANDLER_H
#define STREAM_RT_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>
#include <lfqas/lfqueue.h>

typedef struct srh_instance_s srh_instance_t;
typedef struct srh_request_s srh_request_t;
typedef void (*srh_read_handler_pt)(srh_request_t *);
typedef void (*srh_on_conn_close_pt)(srh_request_t *);
typedef void (*srh_init_handler_pt)(void*);

#define srh_buf_size(b) (size_t) (b->end - b->start)

typedef struct srh_buf_s {
  unsigned char *start;
  unsigned char *end;
  size_t total_size;
} srh_buf_t;

typedef struct {
  int sockfd;
  struct sockaddr_in client_addr;
  socklen_t client_addrlen;
  unsigned isudp: 1;
  unsigned is_listener: 1;
  union {
    int max_message_queue;
    lfqueue_t out_queue;
    srh_request_t *out_req;
  };
  srh_instance_t *instance;
  srh_read_handler_pt read_handler;
  srh_on_conn_close_pt on_conn_close_handler;
} srh_event_t;

struct srh_request_s {
  int sockfd;
  srh_buf_t *in_buff;
  srh_buf_t *out_buff;
  srh_event_t *event;
  void* ctx_val;
  // srh_instance_t *instance;
};

struct srh_instance_s {
  int epfd;
  int nevents, n;
  srh_event_t *evts;
  struct epoll_event *ep_events;
  size_t ep_events_sz;
  srh_init_handler_pt init_handler;
  void* init_arg;
};

extern void srh_write_output_buffer(srh_request_t *req, unsigned char* output);
extern void srh_write_output_buffer_l(srh_request_t *req, unsigned char* output, size_t len);
extern srh_instance_t* srh_create_routing_instance(int max_service_port, srh_init_handler_pt init_handler, void *arg );
extern int srh_add_udp_fd(srh_instance_t *instance, int port, srh_read_handler_pt read_handler, int max_message_queue, srh_on_conn_close_pt on_conn_close_handler);
extern int srh_add_tcp_fd(srh_instance_t *instance, int port, srh_read_handler_pt read_handler, int backlog, srh_on_conn_close_pt on_conn_close_handler);
extern int srh_start(srh_instance_t *instance);


#ifdef __cplusplus
}
#endif

#endif
