#ifndef ROUTE_IO_HANDLER_H
#define ROUTE_IO_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>
#include <lfsaq/lfqueue.h>

typedef struct rio_instance_s rio_instance_t;
typedef struct rio_request_s rio_request_t;
typedef void (*rio_read_handler_pt)(rio_request_t *);
typedef void (*rio_on_conn_close_pt)(rio_request_t *);
typedef void (*rio_init_handler_pt)(void*);

#define rio_buf_size(b) (size_t) (b->end - b->start)

typedef struct rio_buf_s {
  unsigned char *start;
  unsigned char *end;
  size_t total_size;
} rio_buf_t;

typedef struct {
  int sockfd;
  struct sockaddr_in client_addr;
  socklen_t client_addrlen;
  unsigned isudp: 1;
  unsigned is_listener: 1;
  union {
    int max_message_queue;
    lfqueue_t out_queue;
    rio_request_t *out_req;
  };
  rio_instance_t *instance;
  rio_read_handler_pt read_handler;
  rio_on_conn_close_pt on_conn_close_handler;
} rio_event_t;

struct rio_request_s {
  int sockfd;
  rio_buf_t *in_buff;
  rio_buf_t *out_buff;
  rio_event_t *event;
  void* ctx_val;
  // rio_instance_t *instance;
};

struct rio_instance_s {
  int epfd;
  int nevents, n;
  rio_event_t *evts;
  struct epoll_event *ep_events;
  size_t ep_events_sz;
  rio_init_handler_pt init_handler;
  void* init_arg;
};

extern void rio_write_output_buffer(rio_request_t *req, unsigned char* output);
extern void rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t len);
extern rio_instance_t* rio_create_routing_instance(int max_service_port, rio_init_handler_pt init_handler, void *arg );
extern int rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int max_message_queue, rio_on_conn_close_pt on_conn_close_handler);
extern int rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog, rio_on_conn_close_pt on_conn_close_handler);
extern int rio_start(rio_instance_t *instance, int with_threads);


#ifdef __cplusplus
}
#endif

#endif
