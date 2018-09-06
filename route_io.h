#ifndef ROUTE_IO_HANDLER_H
#define ROUTE_IO_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif
  
#if defined __GNUC__ || defined __CYGWIN__ || defined __MINGW32__/*Linux*/

#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>
#include "at_thpool.h"

typedef struct rio_instance_s rio_instance_t;
typedef struct rio_request_s rio_request_t;
typedef void (*rio_read_handler_pt)(rio_request_t *);
typedef void (*rio_on_conn_close_pt)(rio_request_t *);
typedef void (*rio_init_handler_pt)(void*);
typedef enum { rio_false, rio_true } rio_bool_t;

#define rio_buf_size(b) (size_t) (b->end - b->start)
#define rio_add_http_fd rio_add_tcp_fd

typedef enum {
  rio_SUCCESS   =   0,
  rio_SOCK_TIMEOUT   =   1,
  rio_ERROR  =   -1,
} rio_state;

typedef struct rio_buf_s {
  unsigned char *start;
  unsigned char *end;
  size_t capacity;
} rio_buf_t;

struct rio_request_s {
  int sockfd;
  struct sockaddr_in client_addr;
  socklen_t client_addr_len;
  unsigned isudp: 1;
  rio_buf_t *inbuf;
  void* ctx_val;
  rio_read_handler_pt read_handler;
  rio_on_conn_close_pt on_conn_close_handler;
  struct epoll_event *epev;
  size_t sz_per_read;
  int force_close; // force close on host side
};

struct rio_instance_s {
  int epfd;
  int nevents;
  // rio_event_t *evts;
  struct epoll_event *ep_events;
  size_t ep_events_sz;
  rio_init_handler_pt init_handler;
  at_thpool_t *thpool;
  void* init_arg;
};

#endif

extern rio_state rio_write_output_buffer(rio_request_t *req, unsigned char* output);
extern rio_state rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t len);
extern rio_instance_t* rio_create_routing_instance(rio_init_handler_pt init_handler, void *arg );
extern int rio_start(rio_instance_t *instance);
extern int rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler,
                          rio_on_conn_close_pt on_conn_close_handler);
extern int rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog,
                          rio_on_conn_close_pt on_conn_close_handler);
extern void rio_set_no_fork(void);
extern void rio_set_max_polling_event(int opt);
extern void rio_set_def_sz_per_read(int opt);
extern void rio_set_rw_timeout(int read_time_ms, int write_time_ms);
extern void rio_set_curr_req_read_sz(rio_request_t *req, int opt);

/** For HTTP OUTPUT **/
#define rio_http_xform_header "Content-Type: application/x-www-form-urlencoded"
#define rio_http_jsonp_header "Content-Type: application/javascript"
#define rio_http_json_header "Content-Type: application/json"
#define rio_http_textplain_header "Content-Type: text/plain"

extern unsigned char* rio_memstr(unsigned char * start, unsigned char *end, char *pattern);

extern rio_bool_t rio_write_http_status(rio_request_t * request, int statuscode);

extern rio_bool_t rio_write_http_header(rio_request_t * request, char* key, char *val);

extern rio_bool_t rio_write_http_header_2(rio_request_t * request, char* keyval);

extern rio_bool_t rio_write_http_header_3(rio_request_t * request, char* keyval, size_t len);

extern rio_bool_t rio_write_http_content(rio_request_t * request,  char* content);

extern rio_bool_t rio_write_http_content_2(rio_request_t * request, char* content, size_t len);

extern rio_bool_t rio_http_getpath(rio_request_t *req, rio_buf_t *buf);

extern rio_bool_t rio_http_getbody(rio_request_t *req, rio_buf_t *buf);

extern rio_bool_t rio_http_get_queryparam(rio_request_t *req, char *key, rio_buf_t *buf);

#ifdef __cplusplus
}
#endif

#endif

