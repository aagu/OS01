#ifndef OS01_POLL_TEST_LWIP_API_H
#define OS01_POLL_TEST_LWIP_API_H

#include <stddef.h>
#include <stdint.h>
#include <lwip/err.h>

struct netconn;
struct netbuf;
typedef uint16_t u16_t;
typedef uint8_t u8_t;

#define NETCONN_COPY 1

err_t netconn_delete(struct netconn *conn);
err_t netconn_recv(struct netconn *conn, struct netbuf **buf);
err_t netconn_write(struct netconn *conn, const void *data, size_t size,
                    u8_t apiflags);
err_t netconn_write_partly(struct netconn *conn, const void *data, size_t size,
                           u8_t apiflags, size_t *bytes_written);
void netbuf_data(struct netbuf *buf, void **data, uint16_t *len);
int netbuf_next(struct netbuf *buf);
void netbuf_delete(struct netbuf *buf);

#endif
