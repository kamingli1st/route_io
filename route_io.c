#if defined _WIN32 || _WIN64 /*Windows*/

#include "route_io.h"
#include <process.h>
#include <stdio.h>
#include <windows.h>
#include <signal.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#pragma warning(disable:4996)

#define ACCEPT_ADDRESS_LENGTH      ((sizeof( struct sockaddr_in) + 16))
#define DEFAULT_READ_BUFFER_SIZE   1024
#define COMPLETION_KEY_NONE        0
#define COMPLETION_KEY_SHUTDOWN    1
#define COMPLETION_KEY_IO          2

#define RIO_MALLOC malloc
#define RIO_STRLEN(p) strlen((char*)p)
#define RIO_ERROR(errmsg) fprintf(stderr, "%s - %d\n", errmsg, GetLastError() )
#define RIO_FREE(p) free(p);p=NULL
#define RIO_FREE_OUT_BUFF \
if(req){\
if(req->out_buff)RIO_FREE(req->out_buff);}

PROCESS_INFORMATION g_pi;
BOOL is_child = FALSE;
static inline int rio_is_peer_closed(size_t n_byte_read) {
  return n_byte_read == 0;
}
static int rio_setlinger(int sockfd, int onoff, int timeout_sec);
static void rio_on_accept(rio_request_t *req);
static void rio_on_recv(rio_request_t *req);
static void rio_writing_buf(rio_request_t *req, rio_buf_t *out_buf);
static void rio_peer_close(rio_request_t *req);
static void rio_process_and_write(rio_request_t *req,  size_t n_byte_read);
static void rio_clear_buffers(rio_request_t *req);
static void rio_conn_closing(rio_request_t *req);
static void rio_after_close(rio_request_t *req);
static void rio_on_iocp(rio_request_t *req, DWORD nbytes);
static rio_request_t* rio_create_request_event(SOCKET listenfd, HANDLE iocp_port, SIZE_T sz_per_read);
static rio_request_t* rio_create_udp_request_event(SOCKET listenfd, HANDLE iocp_port, SIZE_T sz_per_read);
static int rio_run_iocp_worker(rio_instance_t *instance);
static void rio_def_on_conn_close_handler(rio_request_t *req) {
  /*Do nothing*/
}
static HANDLE master_shutdown_ev = 0;

unsigned __stdcall rio_udp_request_thread(void *);
unsigned __stdcall rio_tcp_request_thread(void *);

static void
rio_on_accept(rio_request_t *req) {
  req->next_state = rio_READABLE;
  DWORD ReceiveLen; // Do nothing for this value
  AcceptEx( req->listenfd, req->sock, req->addr_block, 0, ACCEPT_ADDRESS_LENGTH,
            ACCEPT_ADDRESS_LENGTH, &ReceiveLen, (OVERLAPPED*) req );
}

static void
rio_on_recv(rio_request_t *req) {
  setsockopt( req->sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
              (char*)&req->listenfd, sizeof(SOCKET) );

  req->next_state = rio_AFT_READ_AND_WRITABLE;
  ReadFile( (HANDLE)req->sock, req->in_buff->start, req->in_buff->total_size, 0, (OVERLAPPED*)req );
}

static void
rio_writing_buf(rio_request_t *req, rio_buf_t *out_buf) {
  req->trans_buf.Head = (LPVOID)(out_buf->start);
  req->trans_buf.HeadLength = (DWORD) rio_buf_size (req->out_buff);
  req->next_state = rio_DONE_WRITE;
  TransmitFile( req->sock, 0,  0, 0, (LPOVERLAPPED)req, &req->trans_buf, 0 );
}

static void
rio_peer_close(rio_request_t *req) {
  req->on_conn_close_handler(req);
  req->ctx_val = NULL;
  rio_conn_closing(req);
  if (req->out_buff) {
    RIO_FREE(req->out_buff);
    req->out_buff = NULL;
  }
//  shutdown( req->sock, SD_BOTH );
//  closesocket(req->sock );
}

static void
rio_process_and_write(rio_request_t *req,  size_t n_byte_read) {
  if ( rio_is_peer_closed( n_byte_read ) ) {
    rio_peer_close(req);
  } else {
    req->in_buff->end = req->in_buff->start + n_byte_read;
    unsigned tid;
    HANDLE thread_hdl = (HANDLE)_beginthreadex(NULL, 0, rio_tcp_request_thread, req, 0, &tid);
    if (thread_hdl == 0) {
      fprintf(stderr, "Error while creating the thread: %d\n", GetLastError());
    }
    /*Detach thread*/
    CloseHandle(thread_hdl);
  }
}

static void
rio_clear_buffers(rio_request_t *req) {
  ZeroMemory( req->addr_block, ACCEPT_ADDRESS_LENGTH * 2 );
  req->in_buff->end = req->in_buff->start;
  ZeroMemory( &req->trans_buf, sizeof(TRANSMIT_FILE_BUFFERS) );
  RIO_FREE_OUT_BUFF;
}

static void
rio_conn_closing(rio_request_t *req) {
  req->next_state = rio_PEER_CLOSED;
  TransmitFile( req->sock, 0, 0, 0, (LPOVERLAPPED)req, 0,  TF_DISCONNECT | TF_REUSE_SOCKET );
}

static void
rio_after_close(rio_request_t *req) {
  rio_clear_buffers(req);
  rio_on_accept(req);
}

static void
rio_on_iocp(rio_request_t *req, DWORD nbytes) {
  switch ( req->next_state )
  {
  case rio_READABLE:
    rio_on_recv(req);
    break;
  case rio_AFT_READ_AND_WRITABLE:
    rio_process_and_write( req, nbytes );
    break;
  case rio_DONE_WRITE:
    if (req->out_buff) {
      RIO_FREE(req->out_buff);
      req->out_buff = NULL;
    }
    req->next_state = rio_READABLE;
    req->next_state = rio_AFT_READ_AND_WRITABLE;
    ReadFile( (HANDLE)req->sock, req->in_buff->start, req->in_buff->total_size, 0, (OVERLAPPED*)req );
    break;
  case rio_PEER_CLOSED:
    rio_after_close(req);
    break;
  }
}

static rio_request_t*
rio_create_request_event(SOCKET listenfd, HANDLE iocp_port, SIZE_T sz_per_read) {
  rio_request_t *req = (rio_request_t*) RIO_MALLOC(sizeof(rio_request_t));
  req->ovlp.Internal = 0;
  req->ovlp.InternalHigh = 0;
  req->ovlp.Offset = 0;
  req->ovlp.OffsetHigh = 0;
  req->ovlp.hEvent = 0;
  req->next_state = rio_IDLE;
  req->listenfd = listenfd;
  // int optval = 1;

  sz_per_read = sz_per_read ? sz_per_read : DEFAULT_READ_BUFFER_SIZE;
  rio_buf_t *buf = (rio_buf_t *)RIO_MALLOC(sizeof(rio_buf_t) + sz_per_read );
  buf->total_size = sz_per_read;
  buf->start = buf->end = ((u_char*) buf) + sizeof(rio_buf_t);

  req->in_buff = buf;

  ZeroMemory( req->addr_block, ACCEPT_ADDRESS_LENGTH * 2 );
  // ZeroMemory( read_buf, req->sz_per_read );
  // myRequest.reserve( DEFAULT_READ_BUFFER_SIZE );
  ZeroMemory( &req->trans_buf, sizeof(TRANSMIT_FILE_BUFFERS) );

  req->sock = WSASocket( PF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0,  WSA_FLAG_OVERLAPPED );

  // Associate the client socket with the I/O Completion Port.
  if (CreateIoCompletionPort( (HANDLE)req->sock, iocp_port, COMPLETION_KEY_IO, 0 ) == NULL)  {
    fprintf(stderr, "Error while creating event %d\n", GetLastError());
    RIO_FREE(req);
    return NULL;
  }
  rio_on_accept(req);
  return req;
}

static rio_request_t*
rio_create_udp_request_event(SOCKET listenfd, HANDLE iocp_port, SIZE_T sz_per_read) {
  int rc;
  rio_request_t *req = (rio_request_t*) RIO_MALLOC(sizeof(rio_request_t));
  req->listenfd = listenfd;
  req->ovlp.Internal = 0;
  req->ovlp.InternalHigh = 0;
  req->ovlp.Offset = 0;
  req->ovlp.OffsetHigh = 0;
  req->ovlp.hEvent = 0;
  req->isudp = 1;
  req->ctx_val = NULL;
  req->out_buff = NULL;

  sz_per_read = sz_per_read ? sz_per_read : DEFAULT_READ_BUFFER_SIZE;

  rio_buf_t *in_buff = (rio_buf_t*)RIO_MALLOC( sizeof(rio_buf_t) + (sz_per_read * sizeof(unsigned char)) );
  in_buff->end = in_buff->start = ((u_char*) in_buff) + sizeof(rio_buf_t);
  in_buff->total_size = sz_per_read;
  req->in_buff = in_buff;
  req->client_addr_len = sizeof(req->client_addr);
//  ZeroMemory( &req->client_addr, req->client_addr_len );
  req->next_state = rio_READABLE;
  if (!PostQueuedCompletionStatus(iocp_port, 0, (ULONG_PTR)COMPLETION_KEY_IO, &req->ovlp)) {
    if ((rc = WSAGetLastError()) != WSA_IO_PENDING)
      fprintf(stderr, "PostQueuedCompletionStatus error: %d\r\n", rc);
  }
  return req;
}

unsigned __stdcall
rio_udp_request_thread(void *arg) {
  int rc;
  DWORD out_sz;
  WSABUF udpbuf;
  rio_request_t *req = (rio_request_t*)arg;
  req->read_handler(req);
  if (req->out_buff) {
    if ( (out_sz = rio_buf_size(req->out_buff)) ) {
      udpbuf.buf = (char*) req->out_buff->start;
      udpbuf.len = out_sz;
      req->next_state = rio_DONE_WRITE;
      if (WSASendTo(req->listenfd, &udpbuf, 1,
                    &out_sz, 0, (SOCKADDR *) &req->client_addr,
                    req->client_addr_len, &req->ovlp, NULL) != 0 ) {
        if ((rc = WSAGetLastError()) != WSA_IO_PENDING) {
          fprintf(stderr, "WSARecvFrom error:%d, sock:%d, bytesRead:%d\r\n", rc, req->listenfd, out_sz);
        }
      }
    }
  } else {
    req->next_state = rio_READABLE;
    if (!PostQueuedCompletionStatus(req->iocp, 0, (ULONG_PTR)COMPLETION_KEY_IO, &req->ovlp)) {
      if ((rc = WSAGetLastError()) != WSA_IO_PENDING) {
        fprintf(stderr, "PostQueuedCompletionStatus error: %d\r\n", rc);
      }
    }
  }
  return 0;
}

unsigned __stdcall
rio_tcp_request_thread(void *arg) {
  rio_request_t *req = (rio_request_t*)arg;
  req->read_handler(req);
  if ( req->out_buff ) {
    rio_writing_buf(req, req->out_buff);
  }
  // req->in_buff->end = req->in_buff->start;
  return 0;
}

static int
rio_run_iocp_worker(rio_instance_t *instance) {
  BOOL rc_status;
  DWORD nbytes, dwIoControlCode = SIO_RCVALL;
  unsigned int optval = 1;
  ULONG_PTR CompKey;
  rio_request_t *p_req;
  int err_retry = 30;
  WSABUF udpbuf;
  DWORD udpflag = 0;

  for (;;) {
    rc_status = GetQueuedCompletionStatus( (HANDLE)instance->iocp, &nbytes, &CompKey, (LPOVERLAPPED *) &p_req, INFINITE );

    if ( 0 == rc_status ) {
      // An error occurred; reset to a known state.
      if ( ERROR_MORE_DATA != (rc_status = WSAGetLastError()) ) {
        perror("Erro GET QUEUE");
        fprintf(stderr, "WSAIotcl(%ul) failed with error code %d\n", dwIoControlCode, WSAGetLastError());
        if ( p_req ) {
          rio_peer_close(p_req);
        }
      } else {
//        ioctlsocket(p_req->listenfd, FIONREAD, &nbytes);
        DWORD new_nbytes = nbytes * 2;

        rio_buf_t *new_buf = (rio_buf_t*) RIO_MALLOC(sizeof(rio_buf_t) + new_nbytes);
        new_buf->start = ((u_char*) new_buf) + sizeof(rio_buf_t);
        new_buf->end = (unsigned char*) memcpy(new_buf->start, p_req->in_buff->start, nbytes) + nbytes;
        new_buf->total_size = new_nbytes;
        RIO_FREE(p_req->in_buff);
        p_req->in_buff = new_buf;
        p_req->next_state = rio_AFT_READ_AND_WRITABLE;
        goto RIO_UDP_MODE_DATA_READABLE;
      }

    } else if ( COMPLETION_KEY_IO == CompKey ) {
RIO_UDP_MODE_DATA_READABLE:
      if (p_req->isudp) {
        switch (p_req->next_state) {
        case rio_READABLE:
          udpbuf.buf = (char*) p_req->in_buff->start;
          udpbuf.len = p_req->in_buff->total_size;
          if (WSARecvFrom(p_req->listenfd, &udpbuf, 1, (LPDWORD)&nbytes,
                          (LPDWORD)&udpflag, (struct sockaddr*)&p_req->client_addr,
                          &p_req->client_addr_len, &p_req->ovlp, NULL) != 0) {
            if ((rc_status = WSAGetLastError()) != WSA_IO_PENDING) {
              fprintf(stderr, "WSARecvFrom error:%d, sock:%d, bytesRead:%d\r\n", rc_status, p_req->listenfd, nbytes);
            }
          }
          p_req->next_state = rio_AFT_READ_AND_WRITABLE;
          break;
        case rio_AFT_READ_AND_WRITABLE:
          p_req->next_state = rio_IDLE;
          if (nbytes > 0) {
            p_req->in_buff->end = p_req->in_buff->start + nbytes;
            unsigned udpthreadid;
            HANDLE udp_thread_hdl = (HANDLE)_beginthreadex(NULL, 0, rio_udp_request_thread, p_req, 0, &udpthreadid);
            if (udp_thread_hdl == 0) {
              fprintf(stderr, "Error while creating the thread: %d\n", GetLastError());
            }
            /*Detach thread*/
            CloseHandle(udp_thread_hdl);
          }
          break;
        case rio_DONE_WRITE:
          p_req->on_conn_close_handler(p_req);
          if (p_req->out_buff) {
            RIO_FREE(p_req->out_buff);
            p_req->out_buff = NULL;
          }

          p_req->ctx_val = NULL;
          p_req->next_state = rio_READABLE;
          goto RIO_UDP_MODE_DATA_READABLE;
          break;
        }
      } else {
        rio_on_iocp( p_req,  nbytes );
      }
    } else if ( COMPLETION_KEY_SHUTDOWN == CompKey ) {
      break;
    }

  }
  return 0;
}

void
rio_write_output_buffer(rio_request_t *req, unsigned char* output) {
  rio_buf_t *buf;
  size_t outsz = RIO_STRLEN(output), curr_size, new_size;
  if (outsz == 0) {
    return ;
  }
  if (req->out_buff == NULL) {
    buf = (rio_buf_t*) RIO_MALLOC(sizeof(rio_buf_t) + outsz);
    if (!buf) {
      RIO_ERROR("malloc");
      return;
    }
    buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
    buf->end = ((u_char *)memcpy( buf->start, output, outsz)) + outsz ;
    buf->total_size = outsz;
    req->out_buff = buf;
  } else {
    curr_size = rio_buf_size(req->out_buff);
    if ( (curr_size + outsz) > req->out_buff->total_size ) {
      new_size = (curr_size + outsz) * 2;
      buf = (rio_buf_t*) RIO_MALLOC(sizeof(rio_buf_t) + new_size );
      if (!buf) {
        RIO_ERROR("malloc");
        return;
      }
      buf->start = ((u_char*) buf) + sizeof(rio_buf_t);
      buf->end = ((u_char*) memcpy(buf->start, req->out_buff->start, curr_size)) + curr_size;
      buf->end = ((u_char*) memcpy(buf->end, output, outsz)) + outsz;
      buf->total_size = new_size;
      RIO_FREE(req->out_buff);
      req->out_buff = buf;
    } else {
      buf = req->out_buff;
      buf->end = ((u_char*) memcpy(buf->end, output, outsz)) + outsz;
    }
  }
}

void
rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t outsz) {
  rio_buf_t *buf;
  size_t curr_size, new_size;
  if (outsz == 0) {
    return ;
  }

  if (req->out_buff == NULL) {
    buf = (rio_buf_t*) RIO_MALLOC(sizeof(rio_buf_t) + outsz);
    if (!buf) {
      RIO_ERROR("malloc");
      return;
    }
    buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
    buf->end = ((u_char *)memcpy( buf->start, output, outsz)) + outsz ;
    buf->total_size = outsz;
    req->out_buff = buf;
  } else {
    curr_size = rio_buf_size(req->out_buff);
    if ( (curr_size + outsz) > req->out_buff->total_size ) {
      new_size = (curr_size + outsz) * 2;
      buf = (rio_buf_t*)  RIO_MALLOC(sizeof(rio_buf_t) + new_size);
      if (!buf) {
        RIO_ERROR("malloc");
        return;
      }
      buf->start = ((u_char*) buf) + sizeof(rio_buf_t);
      buf->end = ((u_char*) memcpy(buf->start, req->out_buff->start, curr_size)) + curr_size;
      buf->end = ((u_char*) memcpy(buf->end, output, outsz)) + outsz;
      buf->total_size = new_size;
      RIO_FREE(req->out_buff);
      req->out_buff = buf;
    } else {
      buf = req->out_buff;
      buf->end = ((u_char*) memcpy(buf->end, output, outsz)) + outsz;
    }
  }
}

static void
rio_interrupt_handler(int signal) {
  TerminateProcess(g_pi.hProcess, 0);
  ExitProcess(0);
}

BOOL WINAPI
console_ctrl_handler(DWORD ctrl) {
  switch ( ctrl )
  {
  case CTRL_C_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    TerminateProcess(g_pi.hProcess, 0);
    return TRUE;
  default:
    return FALSE;
  }
}

rio_instance_t*
rio_create_routing_instance(rio_init_handler_pt init_handler, void *arg ) {
  rio_instance_t *instance;
  TCHAR *cmd_str = GetCommandLine();
  SIZE_T sizeof_cmdline = RIO_STRLEN(cmd_str);
  SIZE_T sizeof_childcmd = sizeof("routeio-child-proc") - 1;
  SIZE_T sizeof_child_cmdline;
  // goto CONTINUE_CHILD_IOCP_PROCESS;
  if (sizeof_cmdline > sizeof_childcmd) {
    TCHAR *p_cmd_str = cmd_str +  sizeof_cmdline - sizeof("routeio-child-proc");

    if (strstr(p_cmd_str, "routeio-child-proc")) {
      goto CONTINUE_CHILD_IOCP_PROCESS;
    } else {
      goto SPAWN_CHILD_PROC;
    }
  } else {

SPAWN_CHILD_PROC:
    // Setup a console control handler: We stop the server on CTRL-C
    SetConsoleCtrlHandler( console_ctrl_handler, TRUE );
    signal(SIGINT, rio_interrupt_handler);
    sizeof_child_cmdline  = (sizeof_cmdline + sizeof_childcmd) * sizeof(TCHAR);
    STARTUPINFO si;
    HANDLE child;
    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    ZeroMemory( &g_pi, sizeof(g_pi) );

    TCHAR *child_cmd_str = (TCHAR*) malloc(sizeof_child_cmdline);
    ZeroMemory(child_cmd_str, sizeof_child_cmdline);

    sprintf(child_cmd_str, "%s %s", cmd_str, "routeio-child-proc");

STREAM_RESTART:
    if ( CreateProcess(
           NULL,
           child_cmd_str, // Child cmd string differentiate by last param
           NULL,
           NULL,
           0,
           CREATE_NO_WINDOW,
           NULL,
           NULL,
           &si,
           &g_pi)  == 0) {
      RIO_ERROR("CreateProcess failed\n");
      ExitProcess(0);
    }
    fprintf(stderr, "%s\n", "Press Ctrl-C to terminate the process....");
    WaitForSingleObject( g_pi.hProcess, INFINITE );
    CloseHandle( g_pi.hProcess );
    CloseHandle( g_pi.hThread );

    goto STREAM_RESTART;
  }

  ExitProcess(0);

CONTINUE_CHILD_IOCP_PROCESS:
  instance = (rio_instance_t*) RIO_MALLOC(sizeof(rio_instance_t));
  instance->init_handler = init_handler;
  instance->init_arg = arg;
  // Initialize the Microsoft Windows Sockets Library
  WSADATA Wsa = {0};
  WSAStartup( MAKEWORD(2, 2), &Wsa );
  // Create a new I/O Completion port, only 1 worker is allowed
  instance->iocp = CreateIoCompletionPort( INVALID_HANDLE_VALUE, 0, 0, 0 );

  if (instance->iocp == NULL) {
    fprintf(stderr, "Error while creating routing instance %d\n", GetLastError());
    RIO_FREE(instance);
    ExitProcess(0);
  }

  return instance;
}

int
rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog,
               SIZE_T size_per_read, rio_on_conn_close_pt on_conn_close_handler) {
  int i, optval = 1, rc;
  rio_request_t *preq;
  SOCKET listenfd;
  if ((listenfd = socket(AF_INET , SOCK_DGRAM , 0 )) == INVALID_SOCKET) {
    fprintf(stderr, "socket(AF_INET , SOCK_DGRAM , 0 ) failed %d\n", WSAGetLastError());
  }

  struct sockaddr_in server_addr = {0};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
  server_addr.sin_port = htons( port );

  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval , sizeof(optval)) < 0) {
    fprintf(stderr, "setsockopt(SO_REUSEADDR) failed %d\n", WSAGetLastError());
  }


  if (bind( listenfd, (struct sockaddr*)&server_addr, sizeof(server_addr) ) != 0 ) {
    fprintf(stderr, "Error while socket binding %d\n", WSAGetLastError());
    return -1;
  }

  if (CreateIoCompletionPort( (HANDLE)listenfd, instance->iocp, COMPLETION_KEY_IO, 0 ) == NULL) {
    fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
    return -1;
  }

  /**Multhread accept event per socket**/
  for (i = 0; i < backlog; i++) {
    if ( (preq = rio_create_udp_request_event(listenfd, instance->iocp, size_per_read) ) == NULL ) {
      fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
      return -1;
    }
    if (on_conn_close_handler == NULL) {
      on_conn_close_handler = rio_def_on_conn_close_handler;
    }
    preq->on_conn_close_handler = on_conn_close_handler;
    preq->read_handler = read_handler;
    preq->iocp = instance->iocp;
  }

  return 0;
}

int
rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler,
               int backlog, SIZE_T size_per_read, rio_on_conn_close_pt on_conn_close_handler) {
  int i, optval = 1;
  rio_request_t *preq;
  SOCKET listenfd = WSASocket( PF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED );

  struct sockaddr_in Addr = {0};
  Addr.sin_family = AF_INET;
  Addr.sin_addr.S_un.S_addr = INADDR_ANY;
  Addr.sin_port = htons( port );

  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval , sizeof(optval)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
  }
#ifdef SO_REUSEPORT
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, (const char*)&optval, sizeof(optval)) < 0)
    perror("setsockopt(SO_REUSEPORT) failed");
#endif
  if (bind( listenfd, (struct sockaddr*)&Addr, sizeof(struct sockaddr_in) ) != 0 ) {
    fprintf(stderr, "Error while socket binding %d\n", WSAGetLastError());
    return -1;
  }

  if (listen( listenfd, backlog ) != 0 ) {
    fprintf(stderr, "Error while socket listening %d\n", WSAGetLastError());
    return -1;
  }

  if (CreateIoCompletionPort( (HANDLE)listenfd, instance->iocp, COMPLETION_KEY_IO, 0 ) == NULL) {
    fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
    return -1;
  }

  /**Multhread accept event per socket**/
  for (i = 0; i < backlog; i++) {
    if ( (preq = rio_create_request_event(listenfd, instance->iocp, size_per_read) ) == NULL ) {
      fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
      return -1;
    }

    if (on_conn_close_handler == NULL) {
      on_conn_close_handler = rio_def_on_conn_close_handler;
    }
    preq->on_conn_close_handler = on_conn_close_handler;
    preq->read_handler = read_handler;
    preq->isudp = 0;
    preq->ctx_val = NULL;
    preq->out_buff = NULL;
  }

  return 0;
}

int
rio_start(rio_instance_t *instance) {
  TCHAR *cmd_str = GetCommandLine();
  SIZE_T sizeof_cmdline = RIO_STRLEN(cmd_str);
  SIZE_T sizeof_childcmd = sizeof("routeio-child-proc") - 1;
  SIZE_T sizeof_child_cmdline;
  if (instance->init_handler) {
    instance->init_handler(instance->init_arg);
  }
  rio_run_iocp_worker(instance);

  return 0;
}

#else /*Linux*/

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
// #define RIO_IS_WRITABLE(ev) lfqueue_size(&ev->out_queue)
#define RIO_STRLEN(p) strlen((char*)p)

#define RIO_WAIT_FOR_READ_WRITE
#define RIO_RELEASE_WAIT_FOR_READ_WRITE

#define RIO_ADD_FD(instance, fd, ee) epoll_ctl(instance->epfd, EPOLL_CTL_ADD, fd, ee)
#define RIO_MODIFY_FD(instance, fd, ee) epoll_ctl(instance->epfd, EPOLL_CTL_MOD, fd, ee)
#define RIO_DEL_FD(instance, fd, ee) \
if(epoll_ctl(instance->epfd, EPOLL_CTL_DEL, fd, ee) == -1) {\
RIO_ERROR("error while del fd"); }

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
static void rio_def_on_conn_close_handler(rio_request_t *req) {
  /*Do nothing*/
}

void *rio_read_udp_handler_spawn(void *req_);
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
rio_write_output_buffer(rio_request_t *req, unsigned char* output) {
  rio_buf_t *buf;
  size_t outsz = RIO_STRLEN(output), curr_size, new_size;
  if (outsz == 0) {
    return ;
  }
  if (req->out_buff == NULL) {
    buf = (rio_buf_t*) RIO_MALLOC(sizeof(rio_buf_t) + outsz);
    if (!buf) {
      RIO_ERROR("malloc");
      return;
    }
    buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
    buf->end = ((u_char *)memcpy( buf->start, output, outsz)) + outsz ;
    buf->total_size = outsz;
    req->out_buff = buf;
  } else {
    curr_size = rio_buf_size(req->out_buff);
    if ( (curr_size + outsz) > req->out_buff->total_size ) {
      new_size = (curr_size + outsz) * 2;
      buf = (rio_buf_t*) RIO_MALLOC(sizeof(rio_buf_t) + new_size );
      if (!buf) {
        RIO_ERROR("malloc");
        return;
      }
      buf->start = ((u_char*) buf) + sizeof(rio_buf_t);
      buf->end = ((u_char*) memcpy(buf->start, req->out_buff->start, curr_size)) + curr_size;
      buf->end = ((u_char*) memcpy(buf->end, output, outsz)) + outsz;
      buf->total_size = new_size;
      RIO_FREE(req->out_buff);
      req->out_buff = buf;
    } else {
      buf = req->out_buff;
      buf->end = ((u_char*) memcpy(buf->end, output, outsz)) + outsz;
    }
  }
}

void
rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t outsz) {
  rio_buf_t *buf;
  size_t curr_size, new_size;
  if (outsz == 0) {
    return ;
  }

  if (req->out_buff == NULL) {
    buf = (rio_buf_t*) RIO_MALLOC(sizeof(rio_buf_t) + outsz);
    if (!buf) {
      RIO_ERROR("malloc");
      return;
    }
    buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
    buf->end = ((u_char *)memcpy( buf->start, output, outsz)) + outsz ;
    buf->total_size = outsz;
    req->out_buff = buf;
  } else {
    curr_size = rio_buf_size(req->out_buff);
    if ( (curr_size + outsz) > req->out_buff->total_size ) {
      new_size = (curr_size + outsz) * 2;
      buf = (rio_buf_t*)  RIO_MALLOC(sizeof(rio_buf_t) + new_size);
      if (!buf) {
        RIO_ERROR("malloc");
        return;
      }
      buf->start = ((u_char*) buf) + sizeof(rio_buf_t);
      buf->end = ((u_char*) memcpy(buf->start, req->out_buff->start, curr_size)) + curr_size;
      buf->end = ((u_char*) memcpy(buf->end, output, outsz)) + outsz;
      buf->total_size = new_size;
      RIO_FREE(req->out_buff);
      req->out_buff = buf;
    } else {
      buf = req->out_buff;
      buf->end = ((u_char*) memcpy(buf->end, output, outsz)) + outsz;
    }
  }
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

  if (!isudp) {
    if (setnonblocking(sockfd) == -1 /*|| setlinger(sockfd, 1, 3) == -1 || settimeout(sockfd, 1000, 1000) == -1 */) {
      RIO_ERROR("Error while creating fd");
      return -1;
    }
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

void *
rio_read_udp_handler_spawn(void *_req) {
  rio_request_t *req = (rio_request_t*)_req;
  req->read_handler(req);

  if ( req->out_buff ) {
    while (sendto(req->sockfd, req->out_buff->start, req->out_buff->end - req->out_buff->start, 0,
                  (struct sockaddr *) &req->client_addr, req->client_addr_len) == -1 && errno == EINTR) /*Loop till success or error*/;
    req->on_conn_close_handler(req);
    RIO_FREE(req->out_buff);
    RIO_FREE(req->in_buff);
    RIO_FREE(req);
  } else {
    req->on_conn_close_handler(req);
    RIO_FREE(req->in_buff);
    RIO_FREE(req);
  }
  pthread_exit(NULL);
}

void *
rio_read_tcp_handler_spawn(void *req_) {
  rio_request_t *req = (rio_request_t*)req_;
  // rio_instance_t *instance = req->instance;
  int fd;
  rio_buf_t * buf, *new_buf;
  int bytes_read, bytes_send, est_bytes_left = 0;

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
// #if defined _WIN32 || _WIN64
//       ioctlsocket(fd, FIONREAD, &est_bytes_left);
// #else
      ioctl(fd, FIONREAD, &est_bytes_left);
// #endif
    }
  } while (est_bytes_left > 0);

  RIO_TCP_CHECK_TRY(bytes_read, goto REREAD, goto EXIT_REQUEST);

  if ((buf->end - buf->start) == 0) {
    goto EXIT_REQUEST;
  }
  req->in_buff = buf;

  req->read_handler(req);

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
    // if (req->in_buff)RIO_FREE(req->in_buff); << it will be freed at line:930
    if (req->out_buff)RIO_FREE(req->out_buff);
    req->on_conn_close_handler(req);
    RIO_FREE(req);
  }
  pthread_exit(NULL);
}

static int
rio_run_epoll(rio_instance_t *instance) {
  struct epoll_event *ep_events = instance->ep_events;
  struct epoll_event *epev;
  rio_request_t *main_req, *sub_req;
  int i, n;
  int evstate;
  int fd;
  int nbytes = 0;
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
    evstate = epev->events;
    main_req = epev->data.ptr;

    if (evstate & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
      RIO_DEL_FD(instance, main_req->sockfd, epev);
      continue;
    }

    if (main_req->isudp) {
      fd = main_req->sockfd;
      /**UDP**/
      if ( evstate & EPOLLIN  ) {
// #if defined _WIN32 || _WIN64
//         ioctlsocket(fd, FIONREAD, &nbytes);
// #else
        ioctl(fd, FIONREAD, &nbytes);
// #endif
        if (nbytes > 0) {
          buf = (rio_buf_t*) RIO_MALLOC(sizeof(rio_buf_t) + nbytes);
          if (buf == NULL) {
            RIO_ERROR("No Enough memory allocated");
            return -1;
          }
          buf->end = buf->start = ((u_char*) buf) + sizeof(rio_buf_t);
          buf->total_size = nbytes;

          sub_req = (rio_request_t*)RIO_MALLOC(sizeof(rio_request_t));
          memcpy(sub_req, main_req, sizeof(rio_request_t));
          sub_req->sockfd = fd;//fcntl(fd, F_DUPFD, 0);

          while (recvfrom(fd, buf->start, nbytes, 0,
                          (struct sockaddr *) &sub_req->client_addr, &sub_req->client_addr_len) == -1 && errno == EINTR) /*Loop till success or error*/;

          buf->end = buf->start + nbytes;
          sub_req->in_buff = buf;
          pthread_t t;
          if (pthread_create(&t, NULL, rio_read_udp_handler_spawn, sub_req)) {
            RIO_FREE(sub_req->in_buff);
            RIO_FREE(sub_req);
            RIO_ERROR("Error creating thread\n");
            return -1;
          }
          pthread_detach(t);
        }
      }
    } else {
      // Get new connection
      if ((fd = accept( main_req->sockfd, (struct sockaddr *)&main_req->client_addr,
                        &main_req->client_addr_len)) < 0) {
        RIO_ERROR("Error while accepting port\n");
        continue;
      }

      if ( settimeout(fd, 1000, 1000) == -1 ) {
        return -1;
      }

      sub_req = RIO_MALLOC(sizeof(rio_request_t));
      if (sub_req == NULL) {
        RIO_ERROR("No Enough memory allocated");
        return ENOMEM;
      }
      memcpy(sub_req, main_req, sizeof(rio_request_t));
      sub_req->sockfd = fd;

      pthread_t t;
      if (pthread_create(&t, NULL, rio_read_tcp_handler_spawn, sub_req)) {
        RIO_FREE(sub_req);
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
rio_start(rio_instance_t *instance) {
  int r, child_status;
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
    /** Init epoll events **/
    instance->ep_events_sz = instance->nevents * sizeof(struct epoll_event);
    if ((instance->ep_events = (struct epoll_event*) RIO_MALLOC(instance->ep_events_sz)) == NULL) {
      fprintf(stderr, "%s\n", "error malloc");
      sleep(2);
      return -1;
    }

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

int
rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, rio_on_conn_close_pt on_conn_close_handler) {
  int fd;
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
  p_req->sockfd = fd;//fcntl(fd, F_DUPFD, 0);
  p_req->instance = instance;

  /**Master FD has no request needed**/
  p_req->in_buff = NULL;
  p_req->ctx_val = NULL;
  p_req->out_buff = NULL;
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
  p_req->sockfd = fd;
  p_req->instance = instance;

  /**Master FD has no request needed**/
  p_req->in_buff = NULL;
  p_req->ctx_val = NULL;
  p_req->out_buff = NULL;
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
