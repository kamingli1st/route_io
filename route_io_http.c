#include <stdio.h>
#include <string.h>
#include "route_io.h"

unsigned char*
rio_memstr(unsigned char * start, unsigned char *end, char *pattern) {
  size_t len = end - start, ptnlen = strlen(pattern);
  if (len) {
    while ( (start =
               (unsigned char*) memchr(start,
                                       (int)pattern[0],
                                       len) ) ) {
      len = end - start;
      if (len >= ptnlen) {
        if ( memcmp(start, pattern, ptnlen) == 0) {
          return start;
        }
        start++;
        len--;
        continue;
      }
      return NULL;
    }
  }
  return NULL;
}

rio_bool_t
rio_write_http_status(rio_request_t * request, int statuscode) {
  switch (statuscode) {
  case 100:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 100 Continue");
    break;
  case 101:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 101 Switching Protocols");
    break;
  case 200:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 200 Ok");
    break;
  case 201:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 201 Created");
    break;
  case 202:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 202 Accepted");
    break;
  case 203:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 203 Partial Information");
    break;
  case 204:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 204 No Response");
    break;
  case 205:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 205 Reset Content");
    break;
  case 206:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 206 Partial Content");
    break;
  case 301:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 301 Moved");
    break;
  case 302:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 302 Found");
    break;
  case 303:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 303 Method");
    break;
  case 304:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 304 Not Modified");
    break;
  case 305:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 305 Use Proxy");
    break;
  case 400:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 400 Bad Request");
    break;
  case 401:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 401 Unauthorized");
    break;
  case 402:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 402 Payment Required");
    break;
  case 403:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 403 Forbidden");
    break;
  case 404:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 404 Not Found");
    break;
  case 405:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 405 Method Not Allowed");
    break;
  case 406:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 406 Not Acceptable");
    break;
  case 407:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 407 Proxy Authentication Required");
    break;
  case 408:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 408 Request Timeout");
    break;
  case 409:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 409 Conflict");
    break;
  case 410:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 410 Gone");
    break;
  case 411:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 411 Length Required");
    break;
  case 412:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 412 Precondition Failed");
    break;
  case 413:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 413 Request Entity Too Large");
    break;
  case 414:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 414 Request URI Too Large");
    break;
  case 415:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 415 Unsupported Media Type");
    break;
  case 416:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 416 Requested Rage Not Satisfiable");
    break;
  case 417:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 417 Expectation Failed");
    break;
  case 500:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 500 Internal Error");
    break;
  case 501:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 501 Not Implemented");
    break;
  case 502:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 502 Service temporarily overloaded");
    break;
  case 503:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 503 Service Unavailable");
    break;
  case 504:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 504 Gateway Timeout");
    break;
  case 505:
    rio_write_output_buffer(request, (unsigned char*)"HTTP/1.1 505 Http Version Not Supported");
    break;
  default:
    fprintf(stderr, "%s\n", "No http status code found, please write out customly");
    return rio_false;
  }
  rio_write_output_buffer(request, (unsigned char*)"\r\n");
  return rio_true;
}

rio_bool_t
rio_write_http_header(rio_request_t * request, char* key, char *val) {
  rio_write_output_buffer(request, (unsigned char*) key);
  rio_write_output_buffer(request, (unsigned char*) ": ");
  rio_write_output_buffer(request, (unsigned char*) val);
  rio_write_output_buffer(request, (unsigned char*) "\r\n");
  return rio_true;
}

rio_bool_t
rio_write_http_header_2(rio_request_t * request, char* keyval) {
  rio_write_output_buffer(request, (unsigned char*) keyval);
  rio_write_output_buffer(request, (unsigned char*) "\r\n");
  return rio_true;
}


rio_bool_t
rio_write_http_header_3(rio_request_t * request, char* keyval, size_t len) {
  rio_write_output_buffer_l(request, (unsigned char*) keyval, len);
  rio_write_output_buffer(request, (unsigned char*) "\r\n");
  return rio_true;
}

rio_bool_t
rio_write_http_content(rio_request_t * request, char* content) {
  size_t len;
  len = strlen(content);
  rio_write_output_buffer(request, (unsigned char*) "\r\n");
  rio_write_output_buffer_l(request, (unsigned char*) content, len);
  return rio_true;
}

rio_bool_t
rio_write_http_content_2(rio_request_t * request, char* content, size_t len) {
  rio_write_output_buffer(request, (unsigned char*) "\r\n");
  rio_write_output_buffer_l(request, (unsigned char*) content, len);
  return rio_true;
}

rio_bool_t
rio_http_getpath(rio_request_t *req, rio_buf_t *buf) {
  size_t buflen;
  rio_buf_t *pbuf = req->inbuf;
  if (pbuf) {
    buflen = pbuf->end - pbuf->start;
    if ( (buf->start = (unsigned char*) memchr(pbuf->start, '/', buflen) ) && (buf->end = (unsigned char*) memchr(buf->start, ' ', pbuf->end - buf->start )) ) {
      buf->capacity = buf->end - buf->start;
      return rio_true;
    }
  }
  buf->capacity = 0;
  return rio_false;
}

rio_bool_t
rio_http_getbody(rio_request_t *req, rio_buf_t *buf) {
  rio_buf_t *pbuf = req->inbuf;
  if (pbuf) {
    if ( (buf->start = rio_memstr(pbuf->start, pbuf->end, (char*)"\r\n\r\n")) ) {
      buf->start += 4;
      buf->end = pbuf->end;
      buf->capacity = buf->end - buf->start;
      return rio_true;
    } else if ( (buf->start = rio_memstr(pbuf->start, pbuf->end, (char*) "\n\n") ) ) {
      buf->start += 2;
      buf->end = pbuf->end;
      buf->capacity = buf->end - buf->start;
      return rio_true;
    }
  }
  buf->capacity = 0;
  return rio_false;
}

rio_bool_t
rio_http_get_queryparam(rio_request_t *req, char *key, rio_buf_t *buf) {
  rio_http_getpath(req, buf);
  size_t len = buf->capacity, keylen = strlen(key);
  unsigned char *start = buf->start, *end = buf->end;
  char deli = '?';

  if (len && (start = (unsigned char*) memchr(start, (int)deli, len) ) ) {
    deli = '&';
    do {
      if (end != start) {
        start++;
        len = end - start;
        if (len >= keylen) {
          if ( memcmp(start, key, keylen) == 0) {
            start += keylen;
            if ( (end = (unsigned char*) memchr(start, (int)deli, len) ) ) {
              buf->start = start;
              buf->end = --end;
              buf->capacity = end - start;
            } else {
              buf->start = start;
              buf->capacity = buf->end - start;
            }
            return rio_true;
          }
          continue;
        }
        buf->start = buf->end = 0;
        buf->capacity = 0;
        return rio_false;
      }
    } while ( (start = (unsigned char*) memchr(start, (int)deli, len) ) );
  }
  buf->start = buf->end = 0;
  buf->capacity = 0;
  return rio_false;
}
