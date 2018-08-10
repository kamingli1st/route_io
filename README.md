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

void init_instance(void *arg);
void read_handler(rio_request_t *req);

void init_instance(void *arg) {
  printf("%s\n", "start instance");
}

void read_handler(rio_request_t *req) {
  printf("preparing echo back %.*s\n", (int) (req->in_buff->end - req->in_buff->start), req->in_buff->start);
  rio_write_output_buffer_l(req, req->in_buff->start, (req->in_buff->end - req->in_buff->start));
}

void on_conn_close_handler(rio_request_t *req) {
  printf("%s\n", "connection closing");
}

int main(void) {

  rio_instance_t * instance = rio_create_routing_instance(24, NULL, NULL);
  rio_add_udp_fd(instance, 12345/*port*/, read_handler, on_conn_close_handler);
  rio_add_tcp_fd(instance, 3232/*port*/, read_handler, 64, on_conn_close_handler);

  rio_start(instance);

  return 0;
}

```


## for Windows os build

### Recommend to use VS2017 to build

#### Add the sources file route_io.c route_io_http.c route_io.h into VS2017 project solution.

Alternatively, 

#### Download the Dev-C++ IDE - https://sourceforge.net/projects/orwelldevcpp/



## for Mac osx build

#### Add the sources file route_io.c route_io_http.c route_io.h into xcode or any ide


#### You can use any IDE/build tools as you wish, just add route_io.c route_io_http.c route_io.h to your project




