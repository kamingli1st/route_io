#if defined _WIN32 || _WIN64 /*Windows*/
#include "route_io.h"
#include <process.h>
#include <stdio.h>
#include <windows.h>
#include <signal.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <signal.h>

#ifdef _IA64_
#pragma warning (disable: 4311)
#pragma warning (disable: 4312)
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

static int __RIO_MAX_POLLING_EVENT__ = 64; // By default it fork a process
static int __RIO_NO_FORK_PROCESS__ = 0; // By default it fork a process
static int __RIO_DEF_SZ_PER_READ__ = 1024; // By default size, adjustable
static int __RIO_READ_TIMEOUT_MS__ = 0; // By default size, adjustable
static int __RIO_WRITE_TIMEOUT_MS__ = 0; // By default size, adjustable

#pragma warning(disable:4996)
#pragma warning(disable:4244)

#define ACCEPT_ADDRESS_LENGTH      ((sizeof( struct sockaddr_in) + 16))
#define COMPLETION_KEY_IO          2
#define RIO_MALLOC malloc
#define RIO_STRLEN(p) strlen((char*)p)
#define RIO_ERROR(errmsg) \
do { \
fprintf(stderr, "%s\n", errmsg); \
wchar_t *s = NULL; \
FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, \
			NULL, WSAGetLastError(), \
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), \
			(LPWSTR)&s, 0, NULL); \
fprintf(stderr, "%S\n", s); \
LocalFree(s); \
} while (0)

#define RIO_FREE(p) free(p);p=NULL

#if EAGAIN == EWOULDBLOCK
#define RIO_EAGAIN_EBLOCK EAGAIN
#else
#define RIO_EAGAIN_EBLOCK EAGAIN: case EWOULDBLOCK: case WSAEWOULDBLOCK
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
break; \
default: \
RIO_ERROR("RIO_SOCKET_CHECK_TRY"); \
goto_close; \
break;}}

#define RIO_FREE_REQ_ALL(req) \
if(req){\
if (req->inbuf) { \
RIO_FREE(req->inbuf); \
} \
RIO_FREE(req);}

#define RIO_DEL_WSAPOLL(fdarray, nevents, arrid) \
memmove(&fdarray[arrid], &fdarray[arrid + 1], (nevents - arrid - 1) * sizeof(rio_poll_fd_t))

PROCESS_INFORMATION g_pi;
WSANETWORKEVENTS _NetworkEvents;
BOOL is_child = FALSE;
static inline int rio_is_peer_closed(size_t n_byte_read) {
	return n_byte_read == 0;
}
static rio_request_t* rio_create_tcp_request_event(SOCKET, rio_instance_t*);
static rio_request_t* rio_create_udp_request_event(SOCKET, rio_instance_t*);
static void rio_on_tcp_accept(rio_request_t *req, HANDLE iocp_port);
static int set_read_timeout(int fd, int read_timeout_ms);
static int set_write_timeout(int fd, int write_timeout_ms);
static rio_buf_t* rio_realloc_buf(rio_buf_t * buf);
static int rio_run_poll(rio_instance_t *);

static inline int rio_min(DWORD a, DWORD b) { return (a < b) ? a : b; }
static void rio_def_on_conn_close_handler(rio_request_t *req) {
	/*Do nothing*/
}
static short rio_get_port_from_socket(int sock) {
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	if (getsockname(sock, (struct sockaddr *)&sin, &len) == -1) {
		RIO_ERROR("getsockname");
	}
	else {
		return ntohs(sin.sin_port);
	}
	return -1;
}
static HANDLE master_shutdown_ev = 0;
void rio_read_udp_handler_queue(void *);
void rio_read_tcp_handler_queue(void *);

// static void
// rio_on_tcp_accept(rio_request_t *req, HANDLE iocp_port) {
// 	DWORD ReceiveLen; // Do nothing for this value

// 	req->accept_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
// 	if (req->accept_sock == INVALID_SOCKET) {
// 		RIO_ERROR("* error creating accept socket!");
// 		return;
// 	}

// 	// // Associate the client socket with the I/O Completion Port.
// 	// if (CreateIoCompletionPort((HANDLE)req->accept_sock, iocp_port, COMPLETION_KEY_IO, 0) == NULL) {
// 	// 	fprintf(stderr, "Error while creating event %d\n", GetLastError());
// 	// 	RIO_FREE(req);
// 	// 	return;
// 	// }
// 	// RIO_FREE_TCP_REQ_IN_OUT(req);
// 	AcceptEx(req->sockfd, req->accept_sock, req->client_addr_iocp, 0, ACCEPT_ADDRESS_LENGTH,
// 	         ACCEPT_ADDRESS_LENGTH, &ReceiveLen, (LPOVERLAPPED)req);

// }
// static void
// rio_on_udp_accept(rio_request_t *req, HANDLE iocp_port) {
// 	rio_buf_t *buf;
// 	WSABUF udpbuf;
// 	DWORD nbytes;
// //	if ( (buf = req->inbuf) == NULL) {
// 		buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + __RIO_DEF_SZ_PER_READ__);
// 		if (buf == NULL) {
// 			RIO_ERROR("No Enough memory allocated");
// 			return;
// 		}
// 		buf->end = buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
// 		buf->capacity = __RIO_DEF_SZ_PER_READ__;
// 		req->inbuf = buf;
// 	}
// 	else if (buf->capacity < __RIO_DEF_SZ_PER_READ__) {
// 		req->inbuf->capacity = __RIO_DEF_SZ_PER_READ__;
// 		buf = req->inbuf = rio_realloc_buf(req->inbuf);
// 	}
// 	//req->curr_state = rio_SOCK_UDP_RECV;
// 	//ZeroMemory(buf->start, buf->capacity);
// 	//buf->end = buf->start;
// 	udpbuf.len = (ULONG)req->sz_per_read;
// 	udpbuf.buf = buf->start;
// 	DWORD udpflag = 0, rc;
// 	rc = WSARecvFrom(req->sockfd, &udpbuf, 1, (LPDWORD)&nbytes,
// 		(LPDWORD)&udpflag, (struct sockaddr*)&req->client_udp_addr,
// 		&req->client_addr_len, &req->ovlp, NULL);

// 	if (rc != 0 && (rc = WSAGetLastError()) != WSA_IO_PENDING) {
// 		RIO_ERROR("WSARecvFrom Error");
// 	}
// }

static rio_request_t*
rio_create_tcp_request_event(SOCKET listenfd, rio_instance_t *instance) {

	rio_request_t *req = (rio_request_t*)RIO_MALLOC(sizeof(rio_request_t));
	req->sockfd = listenfd;
	req->ctx_val = NULL;
	req->inbuf = NULL;
	req->udp_outbuf = NULL;
	req->isudp = 0;
	req->force_close = 0;
	req->sz_per_read = __RIO_DEF_SZ_PER_READ__;
	req->client_addr_len = sizeof(req->client_addr);

	ZeroMemory(&req->client_addr, req->client_addr_len);


	instance->fdarr[instance->nevents].pfd.fd = listenfd;
	instance->fdarr[instance->nevents].pfd.events = POLLRDNORM;
	instance->fdarr[instance->nevents].req = req;
	instance->nevents++;

	return req;
}

static rio_request_t*
rio_create_udp_request_event(SOCKET listenfd, rio_instance_t *instance) {

	rio_request_t *req = (rio_request_t*)RIO_MALLOC(sizeof(rio_request_t));
	req->sockfd = listenfd;
	req->isudp = 1;
	req->force_close = 1; // Ignored
	req->ctx_val = NULL;
	req->inbuf = NULL;
	req->udp_outbuf = NULL;
	req->udp_outbuf = NULL;
	req->sz_per_read = __RIO_DEF_SZ_PER_READ__;
	req->client_addr_len = sizeof(req->client_addr);
	ZeroMemory(&req->client_addr, req->client_addr_len);

	instance->fdarr[instance->nevents].pfd.fd = listenfd;
	instance->fdarr[instance->nevents].pfd.events = POLLRDNORM;
	instance->fdarr[instance->nevents].req = req;
	instance->nevents++;

	return req;
}

void
rio_read_udp_handler_queue(void *_req) {
	rio_request_t *req = (rio_request_t*)_req;
	int retbytes, fd = req->sockfd;
	rio_buf_t *inbuf = req->inbuf;
	size_t sz_per_read = req->sz_per_read, output_sz;
	while ((retbytes = recvfrom(fd, inbuf->start, sz_per_read, 0,
		(struct sockaddr *) &req->client_addr, &req->client_addr_len)) == -1 && errno == EINTR) /*Loop till success or error*/;

	if (retbytes > 0) {
		inbuf->end = inbuf->start + retbytes;
		req->inbuf = inbuf;
		req->read_handler(req);
		if (req->udp_outbuf && (output_sz = rio_buf_size(req->udp_outbuf))) {
			while (sendto(req->sockfd, req->udp_outbuf->start, output_sz, 0,
				(struct sockaddr *) &req->client_addr, req->client_addr_len) == -1 && errno == EINTR) /*Loop till success or error*/;
		}
	}
	if (req) {
		req->on_conn_close_handler(req);
		RIO_FREE_REQ_ALL(req);
	}
}

void
rio_read_tcp_handler_queue(void *req_) {
	int retbytes;
	rio_request_t *req = (rio_request_t*)req_;
	rio_buf_t *inbuf = req->inbuf;
	size_t curr_size, sz_per_read = req->sz_per_read, new_size;

	for (;;) {
		while ((retbytes = recv(req->sockfd, inbuf->end, sz_per_read, 0)) <= 0) {
			RIO_SOCKET_CHECK_TRY(retbytes, goto READ_HANDLER, goto EXIT_REQUEST);
		}
		inbuf->end += retbytes;
	READ_HANDLER:
		req->read_handler(req);

		if (req->force_close) {
			goto EXIT_REQUEST;
		}
		else if (inbuf) {
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
		/*	if (retbytes) {
		shutdown(req->sockfd, SD_BOTH);
		closesocket(req->sockfd);
		}*/
		RIO_FREE_REQ_ALL(req);
	}
}


static rio_buf_t*
rio_realloc_buf(rio_buf_t * buf) {
	size_t curr_size = rio_buf_size(buf);
	rio_buf_t*	new_buf = RIO_MALLOC(sizeof(rio_buf_t) + buf->capacity);
	if (new_buf == NULL) {
		RIO_ERROR("malloc ");
		return NULL;
	}
	new_buf->capacity = buf->capacity;
	new_buf->start = ((u_char*)new_buf) + sizeof(rio_buf_t);
	new_buf->end = ((u_char*)memcpy(new_buf->start, buf->start, curr_size)) + curr_size;
	RIO_FREE(buf);
	return new_buf;
}

static int
rio_run_poll(rio_instance_t *instance) {
	INT ret;
	DWORD i;
	rio_request_t *p_req, *new_req;
	rio_buf_t *buf;
	at_thpool_t *tp = instance->thpool;
	rio_poll_fd_t *fdarray = instance->fdarr;
	unsigned int nevents = instance->nevents;
	WSAPOLLFD *pollev;
	SHORT evstate;
	SOCKET fd;
	size_t sz_per_read;
	for (;;) {
		ret = WSAPoll(fdarray, instance->nevents, 30000);

		if (SOCKET_ERROR == ret) {
			// An error occurred; reset to a known state.
			if (ERROR_MORE_DATA == (ret = WSAGetLastError())) {
				fprintf(stderr, "Kindly expand udp packat read size, it does not fit the default read size\n");
			}

			RIO_ERROR("Error while GETTING POLL");

			/*if ((p_req) && p_req->isudp) {
			ZeroMemory(&p_req->client_addr, p_req->client_addr_len);
			if (!PostQueuedCompletionStatus(instance->iocp, 0, (ULONG_PTR)COMPLETION_KEY_IO, &p_req->ovlp)) {
			if ((rc_status = WSAGetLastError()) != WSA_IO_PENDING)
			RIO_ERROR("PostQueuedCompletionStatus error: ");
			}
			}
			else {
			rio_reset_socket(p_req, instance);
			continue;
			}*/
		}
		else {
			for (i = 0; i < ret; i++) {
				pollev = &fdarray[i].pfd;
				evstate = pollev->revents;
				p_req = fdarray[i].req;

				if (evstate & POLLRDNORM) {
					if (p_req->isudp) {
						fd = p_req->sockfd;
						sz_per_read = __RIO_DEF_SZ_PER_READ__;
						buf = (rio_buf_t*)RIO_MALLOC(sizeof(rio_buf_t) + sz_per_read);
						if (buf == NULL) {
							RIO_ERROR("No Enough memory allocated");
							continue;
						}
						buf->end = buf->start = ((u_char*)buf) + sizeof(rio_buf_t);
						buf->capacity = sz_per_read;

						new_req = (rio_request_t*)RIO_MALLOC(sizeof(rio_request_t));
						memcpy(new_req, p_req, sizeof(rio_request_t));
						new_req->sockfd = fd;//fcntl(fd, F_DUPFD, 0);
						new_req->sz_per_read = sz_per_read;
						new_req->inbuf = buf;
						if (at_thpool_newtask(instance->thpool, rio_read_udp_handler_queue, new_req) < 0) {
							RIO_ERROR("Too many job load, please expand the thread pool size");
							RIO_FREE(new_req->inbuf);
							RIO_FREE(new_req);
						}
					}
					else {
						if (p_req->inbuf == NULL) {
							sz_per_read = __RIO_DEF_SZ_PER_READ__;


							if (__RIO_READ_TIMEOUT_MS__ && set_read_timeout(fd, __RIO_READ_TIMEOUT_MS__) == -1) {
								RIO_ERROR("Error while creating fd");
								return -1;
							}

							if (__RIO_WRITE_TIMEOUT_MS__ && set_write_timeout(fd, __RIO_WRITE_TIMEOUT_MS__) == -1) {
								RIO_ERROR("Error while creating fd");
								return -1;
							}
							new_req = RIO_MALLOC(sizeof(rio_request_t));
							if (new_req == NULL) {
								RIO_ERROR("No Enough memory allocated");
								return ENOMEM;
							}
							memcpy(new_req, p_req, sizeof(rio_request_t));
							if ((fd = accept(p_req->sockfd, (struct sockaddr *)&new_req->client_addr,
								&new_req->client_addr_len)) < 0) {
								RIO_ERROR("Error while accepting port\n");
								continue;
							}

							if (fd == INVALID_SOCKET) {
								RIO_ERROR("Error INVALID ACCPETED SOCKET\n");
								Sleep(2000);
								RIO_FREE(new_req);
								continue;
							}
							new_req->sockfd = fd;
							new_req->sz_per_read = sz_per_read;
							new_req->inbuf = buf = RIO_MALLOC(sizeof(rio_buf_t) + sz_per_read);
							if (buf == NULL) {
								RIO_ERROR("No Enough memory allocated");
								return ENOMEM;
							}
							buf->capacity = sz_per_read;
							buf->start = buf->end = ((u_char*)buf) + sizeof(rio_buf_t);
							if (at_thpool_newtask(instance->thpool, rio_read_tcp_handler_queue, new_req) < 0) {
								fprintf(stderr, "%s\n", "Error Running on thread pool");
								closesocket(new_req->sockfd);
								RIO_FREE_REQ_ALL(new_req);
							}
						}
					}
				}
				else if (evstate & (WSAENETDOWN | WSAEFAULT | WSAEINVAL | WSAENOBUFS)) {
					if (p_req->isudp) {
						fprintf(stderr, "Error/closed on udp port %d\n", rio_get_port_from_socket(p_req->sockfd));
						p_req->on_conn_close_handler(p_req);
					}
					else if (p_req->inbuf == NULL) { // only show the tcp listening port
						fprintf(stderr, "Error on tcp port %d\n", rio_get_port_from_socket(p_req->sockfd));
					}
					goto RIO_DEL_AND_FREE_EVENT;
				}
				else {
					// Keep polling
				}

				continue;

			RIO_DEL_AND_FREE_EVENT:
				instance->nevents--;
				closesocket(p_req->sockfd);
				RIO_FREE_REQ_ALL(p_req);
				RIO_DEL_WSAPOLL(fdarray, nevents, i);
				//memmove(&fdarray[i], &fdarray[i + 1], (nevents - i - 1)*sizeof(rio_poll_fd_t));
			}
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
	int outsz;
	rio_buf_t *buf;
	if (output) {
		outsz = (int)RIO_STRLEN(output);
		if (req->isudp) {
			size_t curr_size, new_size;
			if (outsz == 0) {
				return rio_ERROR;
			}
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
		}
		else {
			while ((retbytes = send(req->sockfd, output, outsz, 0)) <= 0) {
				RIO_SOCKET_CHECK_TRY(retbytes, printf("%s\n", "timeout while sending"); return rio_SOCK_TIMEOUT, printf("%s\n", "peer closed while sending"); return rio_ERROR);
			}
		}
	}
	return rio_SUCCESS;
}

rio_state
rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t outsz) {
	int retbytes;
	rio_buf_t *buf;
	if (output) {
		if (req->isudp) {
			size_t curr_size, new_size;
			if (outsz == 0) {
				return rio_ERROR;
			}
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
		}
		else {
			while ((retbytes = send(req->sockfd, output, (int)outsz, 0)) <= 0) {
				RIO_SOCKET_CHECK_TRY(retbytes, printf("%s\n", "timeout while sending"); return rio_SOCK_TIMEOUT, printf("%s\n", "peer closed while sending"); return rio_ERROR);
			}
		}
	}
	return rio_SUCCESS;
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
		goto CONTINUE_CHILD_PROCESS;
	if (cmd_len > childcmd_len) {
		RIOCMD_CHAR *p_cmd_str = cmd_str + cmd_len - sizeof("routeio-child-proc");

		if (rio_cmdstrstr(p_cmd_str, child_cmd_str)) {
			goto CONTINUE_CHILD_PROCESS;
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

CONTINUE_CHILD_PROCESS:
	instance = (rio_instance_t*)RIO_MALLOC(sizeof(rio_instance_t));
	instance->init_handler = init_handler;
	instance->init_arg = arg;
	instance->fdarr = (rio_poll_fd_t *)RIO_MALLOC(__RIO_MAX_POLLING_EVENT__ * sizeof(rio_poll_fd_t));
	instance->nevents = 0;

	if (instance->fdarr == NULL) {
		RIO_ERROR("Malloc Error");
	}

	ZeroMemory(instance->fdarr, __RIO_MAX_POLLING_EVENT__ * sizeof(rio_poll_fd_t));
	//instance->nevents = 0;
	// Initialize the Microsoft Windows Sockets Library
	WSADATA wsa = { 0 };
	if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
		RIO_ERROR("Startup Error");
	}

	return instance;
}

int
rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler,
	rio_on_conn_close_pt on_conn_close_handler) {

	int optval = 1;
	rio_request_t *preq;
	SOCKET listenfd;
	ULONG nonblockflag = 1;
	if ((listenfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
		RIO_ERROR("socket(AF_INET , SOCK_DGRAM , 0 ) failed");
	}

	struct sockaddr_in server_addr = { 0 };
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	server_addr.sin_port = htons(port);

	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(optval)) < 0) {
		fprintf(stderr, "setsockopt(SO_REUSEADDR) failed %d\n", WSAGetLastError());
	}

	/*if (ioctlsocket(listenfd, FIONBIO, &nonblockflag) == SOCKET_ERROR) {
	RIO_ERROR("FIONBIO");
	}*/

	// if (__RIO_READ_TIMEOUT_MS__ && set_read_timeout(listenfd, __RIO_READ_TIMEOUT_MS__) == -1) {
	// 	RIO_ERROR("Error while creating fd");
	// 	return -1;
	// }

	// if (__RIO_WRITE_TIMEOUT_MS__ && set_write_timeout(listenfd, __RIO_WRITE_TIMEOUT_MS__) == -1) {
	// 	RIO_ERROR("Error while creating fd");
	// 	return -1;
	// }

	if (bind(listenfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
		fprintf(stderr, "Error while socket binding %d\n", WSAGetLastError());
		return -1;
	}


	//for (n = 0; n < __RIO_MAX_POLLING_EVENT__; n++) {
	if ((preq = rio_create_udp_request_event(listenfd, instance)) == NULL) {
		fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
		return -1;
	}

	if (on_conn_close_handler == NULL) {
		on_conn_close_handler = rio_def_on_conn_close_handler;
	}
	preq->on_conn_close_handler = on_conn_close_handler;
	preq->read_handler = read_handler;
	//}
	return 0;
}

int
rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler,
	int backlog, rio_on_conn_close_pt on_conn_close_handler) {

	int optval = 1;
	rio_request_t *preq;
	ULONG nonblockflag = 1;

	SOCKET listenfd;

	listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenfd == INVALID_SOCKET) {
		RIO_ERROR("socket(AF_INET , SOCK_STREAM , IPPROTO_TCP ) failed");
	}

	struct sockaddr_in Addr = { 0 };
	Addr.sin_family = AF_INET;
	Addr.sin_addr.S_un.S_addr = INADDR_ANY;
	Addr.sin_port = htons(port);

	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(optval)) < 0) {
		perror("setsockopt(SO_REUSEADDR) failed");
	}


	/*if (ioctlsocket(listenfd, FIONBIO, &nonblockflag) == SOCKET_ERROR) {
	RIO_ERROR("FIONBIO");
	}*/


	// if (__RIO_READ_TIMEOUT_MS__ && set_read_timeout(listenfd, __RIO_READ_TIMEOUT_MS__) == -1) {
	// 	RIO_ERROR("Error while creating fd");
	// 	return -1;
	// }

	// if (__RIO_WRITE_TIMEOUT_MS__ && set_write_timeout(listenfd, __RIO_WRITE_TIMEOUT_MS__) == -1) {
	// 	RIO_ERROR("Error while creating fd");
	// 	return -1;
	// }

	if (bind(listenfd, (struct sockaddr*)&Addr, sizeof(struct sockaddr_in)) != 0) {
		fprintf(stderr, "Error while socket binding %d\n", WSAGetLastError());
		return -1;
	}

	if (listen(listenfd, backlog) != 0) {
		fprintf(stderr, "Error while socket listening %d\n", WSAGetLastError());
		return -1;
	}


	if ((preq = rio_create_tcp_request_event(listenfd, instance)) == NULL) {
		fprintf(stderr, "Error while creating tcp iocp %d\n", GetLastError());
		return -1;
	}

	if (on_conn_close_handler == NULL) {
		on_conn_close_handler = rio_def_on_conn_close_handler;
	}
	preq->on_conn_close_handler = on_conn_close_handler;
	preq->read_handler = read_handler;

	return 0;
}

static int
set_read_timeout(int fd, int recv_timeout_ms) {
	struct timeval recv_tmout_val;
	recv_tmout_val.tv_sec = (recv_timeout_ms >= 1000) ? recv_timeout_ms / 1000 : 0; // Default 1 sec time out
	recv_tmout_val.tv_usec = (recv_timeout_ms % 1000) * 1000;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&recv_tmout_val,
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
	send_tmout_val.tv_usec = (write_timeout_ms % 1000) * 1000;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_tmout_val,
		sizeof(send_tmout_val)) < 0) {
		RIO_ERROR("setsockopt send_tmout_val failed\n");
		return -1;
	}

	return 0;
}

int
rio_start(rio_instance_t *instance, unsigned int n_concurrent_threads) {

	instance->thpool = at_thpool_create(n_concurrent_threads);
	if (instance->init_handler) {
		instance->init_handler(instance->init_arg);
	}
	rio_run_poll(instance);

	return 0;
}
#endif