#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

enum {
    NET_CONNECTING,
    NET_OPEN,
    NET_CLOSED,
};

typedef struct {
    intptr_t fd;      /* posix fd or winsock SOCKET */
    int state;
} net_t;

/* starts a non-blocking connect; returns 0 on start, -1 on immediate failure */
int net_connect(net_t *net, const char *host, const char *port);

/* progresses a pending connect; call every frame while NET_CONNECTING */
void net_service_connect(net_t *net);

/* returns bytes written, 0 if the socket would block, -1 on fatal error */
int net_send(net_t *net, const void *buf, size_t len);

/* returns bytes read, 0 if nothing available, -1 on close/fatal error */
int net_recv(net_t *net, void *buf, size_t len);

void net_close(net_t *net);

#endif
