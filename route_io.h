#ifndef ROUTE_IO_HANDLER_H
#define ROUTE_IO_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif
#if defined _WIN32 || _WIN64

#include <winsock2.h>
#include <mswsock.h>
#include <shlwapi.h>
#include "at_thpool.h"

#pragma comment(lib,"ws2_32")   // Standard socket API.
#pragma comment(lib,"mswsock")  // AcceptEx, TransmitFile, etc,.
#pragma comment(lib,"shlwapi")  // UrlUnescape.
typedef struct rio_request_s rio_request_t;
typedef void (*rio_read_handler_pt)(rio_request_t *);
typedef void (*rio_on_conn_close_pt)(rio_request_t *);
typedef void (*rio_init_handler_pt)(void*);
typedef enum { rio_false, rio_true } rio_bool_t;

#define rio_buf_size(b) (SIZE_T) (b->end - b->start)
#define rio_add_http_fd rio_add_tcp_fd

typedef enum {
  rio_READABLE   =   1,
  rio_WRITABLE  =   2,
  rio_DONE_WRITE = 3,
  rio_PEER_CLOSED = 4,
  rio_FORCE_CLOSE = 5,
  rio_IDLE = 6,
} rio_state;

typedef struct rio_buf_s {
  unsigned char *start;
  unsigned char *end;
  size_t capacity;
} rio_buf_t;

struct rio_request_s {
  OVERLAPPED ovlp;
  SOCKET listenfd;
  union {
    BYTE addr_block[ ((sizeof( struct sockaddr_in) + 16)) * 2 ];
    struct sockaddr_in client_addr;
  };

  union {
    SOCKET sock;
    intptr_t client_addr_len;
  };
  rio_state next_state;
  rio_buf_t *in_buff;
  rio_buf_t *out_buff;
 // HANDLE iocp;
  rio_read_handler_pt read_handler;
  rio_on_conn_close_pt on_conn_close_handler;
  intptr_t isudp: 1;
  void* ctx_val;
  SIZE_T sz_per_read;
  intptr_t force_close; // force close on host side
};

typedef struct {
  HANDLE iocp;
  rio_init_handler_pt init_handler;
  void* init_arg;
  at_thpool_t *thpool;
} rio_instance_t;


#endif

extern void rio_write_output_buffer(rio_request_t *req, unsigned char* output);
extern void rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t len);
extern rio_instance_t* rio_create_routing_instance(rio_init_handler_pt init_handler, void *arg );
extern intptr_t rio_start(rio_instance_t *instance, size_t n_concurrent_threads);
extern intptr_t rio_add_udp_fd(rio_instance_t *instance, intptr_t port, rio_read_handler_pt read_handler,
                          rio_on_conn_close_pt on_conn_close_handler);
extern intptr_t rio_add_tcp_fd(rio_instance_t *instance, intptr_t port, rio_read_handler_pt read_handler, intptr_t backlog,
                          rio_on_conn_close_pt on_conn_close_handler);
extern void rio_set_no_fork(void);
extern void rio_set_max_polling_event(intptr_t opt);
extern void rio_set_def_sz_per_read(intptr_t opt);
extern void rio_set_curr_req_read_sz(rio_request_t* req, intptr_t opt);

/** For HTTP OUTPUT **/
#define rio_http_xform_header "Content-Type: application/x-www-form-urlencoded"
#define rio_http_jsonp_header "Content-Type: application/javascript"
#define rio_http_json_header "Content-Type: application/json"
#define rio_http_textplain_header "Content-Type: text/plain"

extern unsigned char* rio_memstr(unsigned char * start, unsigned char *end, char *pattern);

extern rio_bool_t rio_write_http_status(rio_request_t * request, intptr_t statuscode);

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

