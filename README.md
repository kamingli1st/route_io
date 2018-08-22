# route_io
async route tcp/udp data to your c/c++ function, create one instance to handler different protocol and port at one time


## For Linux OS build

## Installation

```bash
mkdir build

cd build

cmake ..

make

sudo make install

```



## Uninstallation

```bash
cd build

sudo make uninstall

```


## Example to run
```c

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "route_io.h"

void read_handler(rio_request_t *req);
void on_conn_close_handler(rio_request_t *req);
void init_instance(void *arg);

void init_instance(void *arg) {
  fprintf(stderr, "%s\n", "start instance");
}

void read_handler(rio_request_t *req) {
  /** To varable context reference until the connection closed, it will trigger free context handler **/
  if (req->ctx_val == NULL) {
    req->ctx_val = malloc(1024 * sizeof(char));
  }
  // printf("%d,  %.*s\n", i++, (int) (req->in_buff->end - req->in_buff->start), req->in_buff->start);
  rio_write_output_buffer_l(req, req->in_buff->start, (req->in_buff->end - req->in_buff->start));
  rio_write_output_buffer(req, (unsigned char*)"\n");
}

void on_conn_close_handler(rio_request_t *req) {
  if (req->ctx_val) {
    free(req->ctx_val);
  }
  fprintf(stderr, "%s\n", "Connection closing");
}

int main(void) {

  rio_instance_t * instance = rio_create_routing_instance(init_instance, NULL);
#if defined _WIN32 || _WIN64 /*Windows*/
  rio_add_udp_fd(instance, 12345, read_handler, 64, 1024, on_conn_close_handler);
  rio_add_tcp_fd(instance, 3232, read_handler, 64, 1024, on_conn_close_handler);
#else
  rio_add_udp_fd(instance, 12345, read_handler, on_conn_close_handler);
  rio_add_tcp_fd(instance, 3232, read_handler, 64, on_conn_close_handler);
#endif
  rio_start(instance);

  return 0;
}


```

## Extra Configuration on route io on run time
### reconfig the polling events

```c

extern void rio_set_max_polling_event(int opt);

```

### set a default read size for every new connection, if not set, it will based on built in default size which is 1024

```c

extern void rio_set_def_sz_per_read(int opt);

```

### reset current connection read size for single connection session while reading, by default is based on default size

```c
extern void rio_set_curr_req_read_sz(rio_request_t* req, int opt);

```

### to tell system not to fork a process to run the IO, it is not recommended as parent process down or corrupted, the program will be exited.

```c
extern void rio_set_no_fork(void);

```


## for Windows os build

### Recommend to use VS2017 to build

#### Add the sources file route_io.c route_io_http.c route_io.h into VS2017 project solution.

Alternatively, 

#### Download the Dev-C++ IDE - https://sourceforge.net/projects/orwelldevcpp/



## for Mac osx build

#### Add the sources file route_io.c route_io_http.c route_io.h into xcode or any ide


#### You can use any IDE/build tools as you wish, just add route_io.c route_io_http.c route_io.h to your project




