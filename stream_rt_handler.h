#ifndef STREAM_RT_HANDLER_H
#define STREAM_RT_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>
#include "lfqueue/lfqueue.h"

typedef struct srh_instance_s srh_instance_t;
typedef struct srh_request_s srh_request_t;
typedef void (*srh_read_handler_pt)(srh_request_t *);

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
    lfqueue_t out_queue;
    srh_request_t *out_req;
  };
  srh_instance_t *instance;
  srh_read_handler_pt read_handler;
} srh_event_t;

struct srh_request_s {
  int sockfd;
  srh_buf_t *in_buff;
  srh_buf_t *out_buff;
  srh_event_t *event;
  // srh_instance_t *instance;
};

struct srh_instance_s {
  int epfd;
  int nevents, n;
  srh_event_t *evts;
  struct epoll_event *ep_events;
  size_t ep_events_sz;
};

extern void srh_set_output_buffer(srh_request_t *req, unsigned char* output);
extern void srh_set_output_buffer_l(srh_request_t *req, unsigned char* output, size_t len);
extern srh_instance_t* srh_create_routing_instance(int max_service_port);
extern int srh_add_udp_fd(srh_instance_t *instance, int port, srh_read_handler_pt read_handler, int max_message_queue);
extern int srh_add_tcp_fd(srh_instance_t *instance, int port, srh_read_handler_pt read_handler, int backlog);
extern int srh_start(srh_instance_t *instance);


#ifdef __cplusplus
}
#endif

#endif
