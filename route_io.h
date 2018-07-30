#ifndef ROUTE_IO_HANDLER_H
#define ROUTE_IO_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined _WIN32 || _WIN64

#include <winsock2.h>
#include <mswsock.h>
#include <shlwapi.h>

#pragma comment(lib,"ws2_32")   // Standard socket API.
#pragma comment(lib,"mswsock")  // AcceptEx, TransmitFile, etc,.
#pragma comment(lib,"shlwapi")  // UrlUnescape.
typedef struct rio_request_s rio_request_t;
typedef void (*rio_read_handler_pt)(rio_request_t *);
typedef void (*rio_on_conn_close_pt)(rio_request_t *);
typedef void (*rio_init_handler_pt)(void*);

#define rio_buf_size(b) (SIZE_T) (b->end - b->start)
#define rio_add_http_fd rio_add_tcp_fd

typedef enum {
  rio_READABLE   =   0,
  rio_AFT_READ_AND_WRITABLE  =   1,
  rio_PEER_CLOSED    =   2,
  rio_IDLE = 3,
  rio_DONE_WRITE = 4,
} rio_state;

typedef struct rio_buf_s {
  unsigned char *start;
  unsigned char *end;
  size_t total_size;
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
    int client_addr_len;
  };
  rio_state next_state;
  rio_buf_t *in_buff;
  rio_buf_t *out_buff;
  union {
    HANDLE iocp;
    TRANSMIT_FILE_BUFFERS trans_buf;
  };
  rio_read_handler_pt read_handler;
  rio_on_conn_close_pt on_conn_close_handler;
  unsigned isudp: 1;
  void* ctx_val;
};

typedef struct {
  HANDLE iocp;
  rio_init_handler_pt init_handler;
  void* init_arg;
} rio_instance_t;

#else

#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>

typedef struct rio_instance_s rio_instance_t;
typedef struct rio_request_s rio_request_t;
typedef void (*rio_read_handler_pt)(rio_request_t *);
typedef void (*rio_on_conn_close_pt)(rio_request_t *);
typedef void (*rio_init_handler_pt)(void*);

#define rio_buf_size(b) (size_t) (b->end - b->start)
#define rio_add_http_fd rio_add_tcp_fd

typedef struct rio_buf_s {
  unsigned char *start;
  unsigned char *end;
  size_t total_size;
} rio_buf_t;

// typedef struct {
//   int sockfd;
//   struct sockaddr_in client_addr;
//   socklen_t client_addrlen;
//   unsigned isudp: 1;
//   // unsigned is_listener: 1;
//   // union {
//   //   int max_message_queue;
//   //   lfqueue_t out_queue;
//   //   rio_request_t *out_req;
//   // };
//   rio_request_t *req;
//   rio_instance_t *instance;
//   rio_read_handler_pt read_handler;
//   rio_on_conn_close_pt on_conn_close_handler;
// } rio_event_t;

struct rio_request_s {
  int sockfd;
  struct sockaddr_in client_addr;
  socklen_t client_addr_len;
  unsigned isudp: 1;
  rio_buf_t *in_buff;
  rio_buf_t *out_buff;
  void* ctx_val;
  rio_instance_t *instance;
  rio_read_handler_pt read_handler;
  rio_on_conn_close_pt on_conn_close_handler;
  struct epoll_event *epev;
};

struct rio_instance_s {
  int epfd;
  int nevents;
  // rio_event_t *evts;
  struct epoll_event *ep_events;
  size_t ep_events_sz;
  rio_init_handler_pt init_handler;
  void* init_arg;
};

#endif

extern void rio_write_output_buffer(rio_request_t *req, unsigned char* output);
extern void rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t len);
extern rio_instance_t* rio_create_routing_instance(rio_init_handler_pt init_handler, void *arg );
extern int rio_start(rio_instance_t *instance);
#if defined _WIN32 || _WIN64
extern int rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog,
                          SIZE_T size_per_read, rio_on_conn_close_pt on_conn_close_handler);
extern int rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog,
                          SIZE_T size_per_read, rio_on_conn_close_pt on_conn_close_handler);
#else
extern int rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler,
                          rio_on_conn_close_pt on_conn_close_handler);
extern int rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog,
                          rio_on_conn_close_pt on_conn_close_handler);

#endif

/** For HTTP OUTPUT **/
#define rio_http_xform_header "Content-Type: application/x-www-form-urlencoded\r\n"
#define rio_http_jsonp_header "Content-Type: application/javascript\r\n"
#define rio_http_json_header "Content-Type: application/json\r\n"
#define rio_http_textplain_header "Content-Type: text/plain\r\n"

extern int rio_write_http_status(rio_request_t * request, int statuscode);

extern int rio_write_http_header(rio_request_t * request, const char* key, const char *val);

extern int rio_write_http_header_2(rio_request_t * request, const char* keyval);

extern int rio_write_http_header_3(rio_request_t * request, char* keyval, size_t len);

extern int rio_write_http_content(rio_request_t * request, const char* content);

extern int rio_write_http_content_2(rio_request_t * request, char* content, size_t len);

#ifdef __cplusplus
}
#endif

#endif

