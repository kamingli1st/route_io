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
#define COMPLETION_KEY_NONE        0
#define COMPLETION_KEY_SHUTDOWN    1
#define COMPLETION_KEY_IO          2

#define RIO_BUFFER_SIZE 1024
#define RIO_MALLOC malloc
#define RIO_STRLEN(p) strlen((char*)p)
#define RIO_ERROR(errmsg) fprintf(stderr, "%s - %d\n", errmsg, GetLastError() )
#define RIO_FREE(p) free(p);p=NULL
#define RIO_FREE_REQ_IN_OUT(req) \
if(req){\
if(req->in_buff){req->in_buff->end = req->in_buff->start; } \
if(req->out_buff){RIO_FREE(req->out_buff);req->out_buff=NULL;}\
rio_set_curr_req_read_sz(req, __RIO_DEF_SZ_PER_READ__);}

static int __RIO_MAX_POLLING_EVENT__ = 128;
static int __RIO_NO_FORK_PROCESS__ = 0; // By default it fork a process
static int __RIO_DEF_SZ_PER_READ__ = RIO_BUFFER_SIZE; // By default size, adjustable
PROCESS_INFORMATION g_pi;
BOOL is_child = FALSE;
static inline int rio_is_peer_closed(size_t n_byte_read) {
	return n_byte_read == 0;
}
static int rio_setlinger(int sockfd, int onoff, int timeout_sec);
static void rio_on_accept(rio_request_t *);
static void rio_on_peek(rio_request_t *);
static void rio_on_recv(rio_request_t*, rio_state);
static void rio_on_send(rio_request_t *, rio_buf_t *, rio_state);
static void rio_peer_close(rio_request_t *);
static void rio_process_and_write(rio_request_t *, DWORD);
static void rio_clear_buffers(rio_request_t *);
static void rio_after_close(rio_request_t *);
static void rio_reset_socket(rio_request_t *req, rio_instance_t *instance);
static rio_request_t* rio_create_tcp_request_event(SOCKET, HANDLE);
static rio_request_t* rio_create_udp_request_event(SOCKET , HANDLE);
static int rio_run_iocp_worker(rio_instance_t *);
static inline int rio_min(DWORD a, DWORD b) { return (a < b) ? a : b; }
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
	RIO_FREE_REQ_IN_OUT(req);
	AcceptEx(req->listenfd, req->sock, req->addr_block, 0, ACCEPT_ADDRESS_LENGTH,
		ACCEPT_ADDRESS_LENGTH, &ReceiveLen, (LPOVERLAPPED)req);
}

static void
rio_on_recv(rio_request_t *req, rio_state st) {
	int rc;
	rio_buf_t *buf = req->in_buff;
	SIZE_T CurrSz = rio_buf_size(buf), NewSize;

	if ((buf->capacity- CurrSz) < req->sz_per_read) {
		NewSize = buf->capacity * 2;
		while ((NewSize - CurrSz) < req->sz_per_read) {
			NewSize *= 2;
		}

		rio_buf_t *newBuf = (rio_buf_t *)RIO_MALLOC(sizeof(rio_buf_t) + (NewSize * sizeof(u_char)));
		newBuf->capacity = NewSize * sizeof(u_char);
		newBuf->start = ((u_char*)newBuf) + sizeof(rio_buf_t);
		newBuf->end = ((u_char*)memcpy(newBuf->start, buf->start, CurrSz)) + CurrSz;
		RIO_FREE(buf);
		buf = req->in_buff = newBuf;
	}

	DWORD IocpFlag = 0;
	WSABUF IocpBuf = { (ULONG) req->sz_per_read, buf->end };
	req->force_close = 0;
	req->next_state = st;
	if(WSARecv(req->sock, &IocpBuf, 1, NULL, &IocpFlag, (LPOVERLAPPED)req, NULL) == SOCKET_ERROR)
	{
		if ((rc = WSAGetLastError()) != WSA_IO_PENDING)
		{
			fprintf(stderr, "WSARecv Error %d", rc);
		}
	}
	//ReadFile( (HANDLE)req->sock, req->in_buff->start, (DWORD) req->in_buff->capacity, 0, (OVERLAPPED*)req );
}

static void
rio_on_send(rio_request_t *req, rio_buf_t *out_buf, rio_state nextst) {
	int rc;
	WSABUF IocpBuf = { (ULONG)rio_buf_size(out_buf), out_buf->start };
	req->next_state = nextst;
	if (WSASend(req->sock, &IocpBuf, 1, NULL, 0, (LPOVERLAPPED)req, NULL) == SOCKET_ERROR)
	{
		if ((rc= WSAGetLastError()) != WSA_IO_PENDING)
		{
			fprintf(stderr, "WSASend Error %d", rc);
		}
	}
}

static void
rio_peer_close(rio_request_t *req) {
	req->on_conn_close_handler(req);
	req->ctx_val = NULL;
	req->next_state = rio_PEER_CLOSED;
	TransmitFile(req->sock, 0, 0, 0, (LPOVERLAPPED)req, 0, TF_DISCONNECT | TF_REUSE_SOCKET);
	//  shutdown( req->sock, SD_BOTH );
	//  closesocket(req->sock );
}

static void
rio_process_and_write(rio_request_t *req, DWORD n_byte_read) {
	if (rio_is_peer_closed(n_byte_read)) {
		rio_peer_close(req);
	}
	else {
		req->in_buff->end = req->in_buff->end + n_byte_read;
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
rio_reset_socket(rio_request_t *req, rio_instance_t *instance) {
	req->sock = WSASocket(PF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);

	// Associate the client socket with the I/O Completion Port.
	if (CreateIoCompletionPort((HANDLE)req->sock, instance->iocp, COMPLETION_KEY_IO, 0) == NULL) {
		fprintf(stderr, "Error while creating event %d\n", GetLastError());
		RIO_FREE(req);
		return;
	}
	rio_after_close(req);
}

static void
rio_after_close(rio_request_t *req) {
	ZeroMemory(req->addr_block, ACCEPT_ADDRESS_LENGTH * 2);
	RIO_FREE_REQ_IN_OUT(req);
	rio_on_accept(req);
}

static void
rio_on_udp_iocp(rio_request_t *req, DWORD nbytes) {
	rio_buf_t * buf = req->in_buff;
	WSABUF udpbuf;
	DWORD udpflag, rc;

RIO_UDP_MODE_SWITCH_STATE:
	switch (req->next_state) {
	case rio_READABLE:
		if (buf->capacity < __RIO_DEF_SZ_PER_READ__) {
			do {
				buf->capacity *= 2;
			} while (buf->capacity < __RIO_DEF_SZ_PER_READ__);
			req->sz_per_read = __RIO_DEF_SZ_PER_READ__;
		}
		udpbuf.len = (ULONG)req->sz_per_read;
		udpbuf.buf = buf->start;
		udpflag = 0;
		req->next_state = rio_WRITABLE;
		rc = WSARecvFrom(req->listenfd, &udpbuf, 1, (LPDWORD)&nbytes,
			(LPDWORD)&udpflag, (struct sockaddr*)&req->client_addr,
			&req->client_addr_len, &req->ovlp, NULL);

		if (rc != 0 && (rc = WSAGetLastError()) != WSA_IO_PENDING) {
#ifdef _WIN64
			fprintf(stderr, "WSARecvFrom error:%d, sock:%lld, bytesRead:%d\r\n", rc, req->listenfd, nbytes);
#else
			fprintf(stderr, "WSARecvFrom error:%d, sock:%Id, bytesRead:%Id\r\n", rc, req->listenfd, nbytes);
#endif
		}
		break;
	case rio_WRITABLE:
		req->next_state = rio_IDLE;
		if (nbytes > 0) {
			req->in_buff->end = req->in_buff->start + nbytes;
			unsigned udpthreadid;
			HANDLE udp_thread_hdl = (HANDLE)_beginthreadex(NULL, 0, rio_udp_request_thread, req, 0, &udpthreadid);
			if (udp_thread_hdl == 0) {
				fprintf(stderr, "Error while creating the thread: %d\n", GetLastError());
			}
			/*Detach thread*/
			CloseHandle(udp_thread_hdl);
			break;
		}
	case rio_DONE_WRITE:
		req->on_conn_close_handler(req);
		RIO_FREE_REQ_IN_OUT(req);
		req->ctx_val = NULL;
		req->next_state = rio_READABLE;
		ZeroMemory(&req->client_addr, req->client_addr_len);
		goto RIO_UDP_MODE_SWITCH_STATE;
		break;
	}
}

static rio_request_t*
rio_create_tcp_request_event(SOCKET listenfd, HANDLE iocp_port) {

	const DWORD defSize = __RIO_DEF_SZ_PER_READ__;

	rio_request_t *req = (rio_request_t*)RIO_MALLOC(sizeof(rio_request_t));
	req->ovlp.Internal = 0;
	req->ovlp.InternalHigh = 0;
	req->ovlp.Offset = 0;
	req->ovlp.OffsetHigh = 0;
	req->ovlp.hEvent = 0;
	req->next_state = rio_IDLE;
	req->listenfd = listenfd;
	req->sz_per_read = defSize;
	req->isudp = 0;
	req->force_close = 0;
	req->ctx_val = NULL;
	req->out_buff = NULL;
	// int optval = 1;
	rio_buf_t *buf = (rio_buf_t *)RIO_MALLOC(sizeof(rio_buf_t) + ((defSize) * sizeof(u_char)) );
	buf->capacity = defSize * sizeof(u_char);
	buf->start = buf->end = ((u_char*) buf) + sizeof(rio_buf_t);
	req->in_buff = buf;

	ZeroMemory(req->addr_block, ACCEPT_ADDRESS_LENGTH * 2);

	req->sock = WSASocket(PF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);


	// Associate the client socket with the I/O Completion Port.
	if (CreateIoCompletionPort((HANDLE)req->sock, iocp_port, COMPLETION_KEY_IO, 0) == NULL) {
		fprintf(stderr, "Error while creating event %d\n", GetLastError());
		RIO_FREE(req);
		return NULL;
	}
	rio_on_accept(req);
	return req;
}

static rio_request_t*
rio_create_udp_request_event(SOCKET listenfd, HANDLE iocp_port) {
	
	int rc;
	const DWORD defSize = __RIO_DEF_SZ_PER_READ__;

	rio_request_t *req = (rio_request_t*)RIO_MALLOC(sizeof(rio_request_t));
	req->listenfd = listenfd;
	req->ovlp.Internal = 0;
	req->ovlp.InternalHigh = 0;
	req->ovlp.Offset = 0;
	req->ovlp.OffsetHigh = 0;
	req->ovlp.hEvent = 0;
	req->isudp = 1;
	req->force_close = 0;
	req->ctx_val = NULL;
	req->out_buff = NULL;
	req->sz_per_read = defSize;

	rio_buf_t *buf = (rio_buf_t *)RIO_MALLOC(sizeof(rio_buf_t) + (defSize * sizeof(u_char)));
	buf->capacity = defSize * sizeof(u_char);
	buf->start = buf->end = ((u_char*)buf) + sizeof(rio_buf_t);

	req->in_buff = buf;
	req->client_addr_len = sizeof(req->client_addr);
	ZeroMemory( &req->client_addr, req->client_addr_len );
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
	SIZE_T out_sz;
	WSABUF udpbuf;
	rio_request_t *req = (rio_request_t*)arg;
	req->read_handler(req);
	if (req->out_buff) {
		if ((out_sz = rio_buf_size(req->out_buff))) {
			udpbuf.buf = (char*)req->out_buff->start;
			udpbuf.len = (ULONG)out_sz;
			req->next_state = rio_DONE_WRITE;
			if (WSASendTo(req->listenfd, &udpbuf, 1,
				(LPDWORD)&out_sz, 0, (SOCKADDR *)&req->client_addr,
				req->client_addr_len, &req->ovlp, NULL) != 0) {
				if ((rc = WSAGetLastError()) != WSA_IO_PENDING) {
#ifdef _WIN64
					fprintf(stderr, "WSASendTo error:%d, sock:%lld, bytesRead:%lld\r\n", rc, req->listenfd, out_sz);
#else
					fprintf(stderr, "WSASendTo error:%d, sock:%Id, bytesRead:%Id\r\n", rc, req->listenfd, out_sz);
#endif
				}
			}
		}
	}
	return 0;
}

unsigned __stdcall
rio_tcp_request_thread(void *arg) {
	rio_request_t *req = (rio_request_t*)arg;
	req->read_handler(req);
	if (req->out_buff) {
		rio_on_send(req, req->out_buff, req->force_close ? rio_FORCE_CLOSE : rio_READABLE);
	}
	else if (req->force_close) {
		rio_on_recv(req, rio_FORCE_CLOSE);
	}
	else {
		rio_on_recv(req, rio_WRITABLE);
	}
	return 0;
}

static int
rio_run_iocp_worker(rio_instance_t *instance) {
	BOOL rc_status;
	DWORD nbytes;
	ULONG_PTR CompKey;
	rio_request_t *p_req;
	//int err_retry = 30;

	for (;;) {
		rc_status = GetQueuedCompletionStatus((HANDLE)instance->iocp, &nbytes, &CompKey, (LPOVERLAPPED *)&p_req, INFINITE);

		if (0 == rc_status) {
			// An error occurred; reset to a known state.
			if (ERROR_MORE_DATA == (rc_status = WSAGetLastError())) {
				fprintf(stderr, "Kindly expand udp packat read size, it does not fit the default read size\n");
			}

			RIO_ERROR("Error while GETTING QUEUE");
			fprintf(stderr, "failed with error code %d\n", WSAGetLastError());

			if ((p_req) && p_req->isudp) {
				ZeroMemory(&p_req->client_addr, p_req->client_addr_len);
				p_req->next_state = rio_READABLE;
				if (!PostQueuedCompletionStatus(instance->iocp, 0, (ULONG_PTR)COMPLETION_KEY_IO, &p_req->ovlp)) {
					if ((rc_status = WSAGetLastError()) != WSA_IO_PENDING)
						fprintf(stderr, "PostQueuedCompletionStatus error: %d\r\n", rc_status);
				}
			}
			else {
				rio_reset_socket(p_req, instance);
				continue;
			}
		}
		else if (COMPLETION_KEY_IO == CompKey) {
			if (p_req->isudp) {
				rio_on_udp_iocp(p_req, nbytes);
			}
			else {
				switch (p_req->next_state)
				{
				case rio_READABLE:
					rio_on_recv(p_req, rio_WRITABLE);
					break;
				case rio_WRITABLE:
					rio_process_and_write(p_req, nbytes);
					break;
				case rio_FORCE_CLOSE:
					p_req->on_conn_close_handler(p_req);
					closesocket(p_req->sock);
					if (!PostQueuedCompletionStatus(instance->iocp, 0, (ULONG_PTR)COMPLETION_KEY_SHUTDOWN, &p_req->ovlp)) {
						if ((rc_status = WSAGetLastError()) != WSA_IO_PENDING)
							fprintf(stderr, "PostQueuedCompletionStatus error: %d\r\n", rc_status);
					}
					break;
				case rio_PEER_CLOSED:
					rio_after_close(p_req);
					break;
				default:
					break;
				}
			}
		}
		else if (COMPLETION_KEY_SHUTDOWN == CompKey) {
			if ((p_req)) {
				rio_reset_socket(p_req, instance);
				continue;
			}
			break;
		}

	}
	return 0;
}

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
rio_set_curr_req_read_sz(rio_request_t *req, int opt) {
	req->sz_per_read = opt;
}

void
rio_write_output_buffer(rio_request_t *req, unsigned char* output) {
	rio_buf_t *buf;
	size_t outsz = RIO_STRLEN(output), curr_size, new_size;
	if (outsz == 0) {
		return;
	}
	if (req->out_buff == NULL) {
		buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + outsz);
		if (!buf) {
			RIO_ERROR("malloc");
			return;
		}
		buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
		buf->end = ((u_char *)memcpy(buf->start, output, outsz)) + outsz;
		buf->capacity = outsz;
		req->out_buff = buf;
	}
	else {
		curr_size = rio_buf_size(req->out_buff);
		if ((curr_size + outsz) > req->out_buff->capacity) {
			new_size = (curr_size + outsz) * 2;
			buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + new_size);
			if (!buf) {
				RIO_ERROR("malloc");
				return;
			}
			buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
			buf->end = ((u_char*)memcpy(buf->start, req->out_buff->start, curr_size)) + curr_size;
			buf->end = ((u_char*)memcpy(buf->end, output, outsz)) + outsz;
			buf->capacity = new_size;
			RIO_FREE(req->out_buff);
			req->out_buff = buf;
		}
		else {
			buf = req->out_buff;
			buf->end = ((u_char*)memcpy(buf->end, output, outsz)) + outsz;
		}
	}
}

void
rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t outsz) {
	rio_buf_t *buf;
	size_t curr_size, new_size;
	if (outsz == 0) {
		return;
	}

	if (req->out_buff == NULL) {
		buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + outsz);
		if (!buf) {
			RIO_ERROR("malloc");
			return;
		}
		buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
		buf->end = ((u_char *)memcpy(buf->start, output, outsz)) + outsz;
		buf->capacity = outsz;
		req->out_buff = buf;
	}
	else {
		curr_size = rio_buf_size(req->out_buff);
		if ((curr_size + outsz) > req->out_buff->capacity) {
			new_size = (curr_size + outsz) * 2;
			buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + new_size);
			if (!buf) {
				RIO_ERROR("malloc");
				return;
			}
			buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
			buf->end = ((u_char*)memcpy(buf->start, req->out_buff->start, curr_size)) + curr_size;
			buf->end = ((u_char*)memcpy(buf->end, output, outsz)) + outsz;
			buf->capacity = new_size;
			RIO_FREE(req->out_buff);
			req->out_buff = buf;
		}
		else {
			buf = req->out_buff;
			buf->end = ((u_char*)memcpy(buf->end, output, outsz)) + outsz;
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
	switch (ctrl)
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
rio_create_routing_instance(rio_init_handler_pt init_handler, void *arg) {
	rio_instance_t *instance;

	//IocpBuf.len = __RIO_SZ_PER_READ__;
	//IocpBuf.buf = RIO_MALLOC(__RIO_SZ_PER_READ__ * sizeof(unsigned char));

#if defined(UNICODE) || defined(_UNICODE)
	typedef WCHAR RIOCMD_CHAR;
#define rio_cmdlen wcslen
#define rio_cmdstrstr wcsstr
	static const RIOCMD_CHAR* child_cmd_str = L"routeio-child-proc";
#else
	typedef char RIOCMD_CHAR;
#define rio_cmdlen strlen
#define rio_cmdstrstr strstr
	static const RIOCMD_CHAR* child_cmd_str = "routeio-child-proc";
#endif

	RIOCMD_CHAR *cmd_str = GetCommandLine();
	SIZE_T cmd_len = rio_cmdlen(cmd_str);
	SIZE_T childcmd_len = rio_cmdlen(child_cmd_str);
	SIZE_T spawn_child_cmd_len = cmd_len + childcmd_len + 1; // 1 for NULL terminator
	if (__RIO_NO_FORK_PROCESS__)
		goto CONTINUE_CHILD_IOCP_PROCESS;
	if (cmd_len > childcmd_len) {
		RIOCMD_CHAR *p_cmd_str = cmd_str + cmd_len - sizeof("routeio-child-proc");

		if (rio_cmdstrstr(p_cmd_str, child_cmd_str)) {
			goto CONTINUE_CHILD_IOCP_PROCESS;
		}
		else {
			goto SPAWN_CHILD_PROC;
		}
	}
	else {

	SPAWN_CHILD_PROC:
		// Setup a console control handler: We stop the server on CTRL-C
		SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
		signal(SIGINT, rio_interrupt_handler);
		STARTUPINFO si;
		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		ZeroMemory(&g_pi, sizeof(g_pi));

		RIOCMD_CHAR *spawn_child_cmd_str = (RIOCMD_CHAR*)RIO_MALLOC(spawn_child_cmd_len * sizeof(RIOCMD_CHAR));
		ZeroMemory(spawn_child_cmd_str, spawn_child_cmd_len * sizeof(RIOCMD_CHAR));

		int i, j;
		for (i = 0; i < cmd_len; i++) {
			spawn_child_cmd_str[i] = cmd_str[i];
		}
		spawn_child_cmd_str[i++] = ' ';

		for (j = 0; j < childcmd_len; i++, j++) {
			spawn_child_cmd_str[i] = child_cmd_str[j];
		}

		spawn_child_cmd_str[i] = '\0';

	STREAM_RESTART:
		if (CreateProcess(
			NULL,
			spawn_child_cmd_str, // Child cmd string differentiate by last param
			NULL,
			NULL,
			0,
			CREATE_NO_WINDOW,
			NULL,
			NULL,
			&si,
			&g_pi) == 0) {
			RIO_ERROR("CreateProcess failed\n");
			Sleep(2000);
			ExitProcess(0);
		}
		fprintf(stderr, "%s\n", "Press Ctrl-C to terminate the process....");
		WaitForSingleObject(g_pi.hProcess, INFINITE);
		CloseHandle(g_pi.hProcess);
		CloseHandle(g_pi.hThread);

		goto STREAM_RESTART;
	}

	ExitProcess(0);

CONTINUE_CHILD_IOCP_PROCESS:
	instance = (rio_instance_t*)RIO_MALLOC(sizeof(rio_instance_t));
	instance->init_handler = init_handler;
	instance->init_arg = arg;
	// Initialize the Microsoft Windows Sockets Library
	WSADATA Wsa = { 0 };
	WSAStartup(MAKEWORD(2, 2), &Wsa);
	// Create a new I/O Completion port, only 1 worker is allowed
	instance->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);

	if (instance->iocp == NULL) {
		fprintf(stderr, "Error while creating routing instance %d\n", GetLastError());
		RIO_FREE(instance);
		ExitProcess(0);
	}

	return instance;
}

int
rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler,
	rio_on_conn_close_pt on_conn_close_handler) {
	int i, optval = 1;
	rio_request_t *preq;
	SOCKET listenfd;
	if ((listenfd = WSASocket(PF_INET, SOCK_DGRAM, IPPROTO_IP, 0, 0, WSA_FLAG_OVERLAPPED)) == INVALID_SOCKET) {
		fprintf(stderr, "socket(AF_INET , SOCK_DGRAM , 0 ) failed %d\n", WSAGetLastError());
	}

	struct sockaddr_in server_addr = { 0 };
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	server_addr.sin_port = htons(port);

	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(optval)) < 0) {
		fprintf(stderr, "setsockopt(SO_REUSEADDR) failed %d\n", WSAGetLastError());
	}


	if (bind(listenfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
		fprintf(stderr, "Error while socket binding %d\n", WSAGetLastError());
		return -1;
	}

	if (CreateIoCompletionPort((HANDLE)listenfd, instance->iocp, COMPLETION_KEY_IO, 0) == NULL) {
		fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
		return -1;
	}

	/**Multhread accept event per socket**/
	for (i = 0; i < __RIO_MAX_POLLING_EVENT__; i++) {
		if ((preq = rio_create_udp_request_event(listenfd, instance->iocp)) == NULL) {
			fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
			return -1;
		}
		if (on_conn_close_handler == NULL) {
			on_conn_close_handler = rio_def_on_conn_close_handler;
		}
		preq->on_conn_close_handler = on_conn_close_handler;
		preq->read_handler = read_handler;
		//preq->iocp = instance->iocp;
		//preq->sock = listenfd;
	}

	return 0;
}

int
rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler,
	int backlog, rio_on_conn_close_pt on_conn_close_handler) {
	int i, optval = 1;
	rio_request_t *preq;
	SOCKET listenfd = WSASocket(PF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);

	struct sockaddr_in Addr = { 0 };
	Addr.sin_family = AF_INET;
	Addr.sin_addr.S_un.S_addr = INADDR_ANY;
	Addr.sin_port = htons(port);

	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(optval)) < 0) {
		perror("setsockopt(SO_REUSEADDR) failed");
	}
#ifdef SO_REUSEPORT
	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, (const char*)&optval, sizeof(optval)) < 0)
		perror("setsockopt(SO_REUSEPORT) failed");
#endif
	if (bind(listenfd, (struct sockaddr*)&Addr, sizeof(struct sockaddr_in)) != 0) {
		fprintf(stderr, "Error while socket binding %d\n", WSAGetLastError());
		return -1;
	}

	if (listen(listenfd, backlog) != 0) {
		fprintf(stderr, "Error while socket listening %d\n", WSAGetLastError());
		return -1;
	}

	if (CreateIoCompletionPort((HANDLE)listenfd, instance->iocp, COMPLETION_KEY_IO, 0) == NULL) {
		fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
		return -1;
	}

	/**Multhread accept event per socket**/
	for (i = 0; i < __RIO_MAX_POLLING_EVENT__; i++) {
		if ((preq = rio_create_tcp_request_event(listenfd, instance->iocp)) == NULL) {
			fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
			return -1;
		}

		if (on_conn_close_handler == NULL) {
			on_conn_close_handler = rio_def_on_conn_close_handler;
		}
		preq->on_conn_close_handler = on_conn_close_handler;
		preq->read_handler = read_handler;
	}

	return 0;
}

int
rio_start(rio_instance_t *instance) {
	if (instance->init_handler) {
		instance->init_handler(instance->init_arg);
	}
	rio_run_iocp_worker(instance);

	return 0;
}
#endif