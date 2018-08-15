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

## Extra Paramater *size_per_read* on windows os
```c
extern int rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog,
                          SIZE_T size_per_read, rio_on_conn_close_pt on_conn_close_handler);
extern int rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog,
                          SIZE_T size_per_read, rio_on_conn_close_pt on_conn_close_handler);
```

### every event connection has *size_per_read* specify, if you set 64 backlog, which mean 64 events has different *size_per_read*, size_per_read is expandable if has more data to read. 


## for Windows os build

### Recommend to use VS2017 to build

#### Add the sources file route_io.c route_io_http.c route_io.h into VS2017 project solution.

Alternatively, 

#### Download the Dev-C++ IDE - https://sourceforge.net/projects/orwelldevcpp/



## for Mac osx build

#### Add the sources file route_io.c route_io_http.c route_io.h into xcode or any ide


#### You can use any IDE/build tools as you wish, just add route_io.c route_io_http.c route_io.h to your project




