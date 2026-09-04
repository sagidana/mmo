#include "net.h"

#include <string.h>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

#define MSG_NOSIGNAL 0
#define SOCKET_CAST SOCKET

static void net_platform_init(void)
{
    static int done = 0;
    WSADATA wsa;

    if (done) return;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    done = 1;
}

static void set_nonblocking(intptr_t fd)
{
    u_long on = 1;

    ioctlsocket((SOCKET)fd, FIONBIO, &on);
}

static int connect_in_progress(void)
{
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

static int would_block(void)
{
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

static void close_socket(intptr_t fd)
{
    closesocket((SOCKET)fd);
}

static int socket_error(intptr_t fd)
{
    int err = 0;
    int err_len = sizeof(err);

    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &err_len) != 0) return 1;
    return err;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SOCKET_CAST int

static void net_platform_init(void)
{
}

static void set_nonblocking(intptr_t fd)
{
    fcntl((int)fd, F_SETFL, fcntl((int)fd, F_GETFL, 0) | O_NONBLOCK);
}

static int connect_in_progress(void)
{
    return errno == EINPROGRESS;
}

static int would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static void close_socket(intptr_t fd)
{
    close((int)fd);
}

static int socket_error(intptr_t fd)
{
    int err = 0;
    socklen_t err_len = sizeof(err);

    if (getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0) return 1;
    return err;
}

#endif

int net_connect(net_t *net, const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result;
    struct addrinfo *rp;
    intptr_t fd = -1;
    int connected;

    net_platform_init();

    net->fd = -1;
    net->state = NET_CLOSED;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &result) != 0) return -1;

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = (intptr_t)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;

        set_nonblocking(fd);

        connected = connect((SOCKET_CAST)fd, rp->ai_addr, (int)rp->ai_addrlen);
        if (connected == 0) break;
        if (connect_in_progress()) break;

        close_socket(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd == -1) return -1;

    net->fd = fd;
    net->state = NET_CONNECTING;
    return 0;
}

void net_service_connect(net_t *net)
{
    fd_set wfds;
    struct timeval tv;

    if (net->state != NET_CONNECTING) return;

    FD_ZERO(&wfds);
    FD_SET((SOCKET_CAST)net->fd, &wfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    if (select((int)net->fd + 1, NULL, &wfds, NULL, &tv) <= 0) return;

    if (socket_error(net->fd) != 0) {
        net_close(net);
        return;
    }
    net->state = NET_OPEN;
}

int net_send(net_t *net, const void *buf, size_t len)
{
    int n;

    if (net->state != NET_OPEN) return -1;

    n = (int)send((SOCKET_CAST)net->fd, buf, (int)len, MSG_NOSIGNAL);
    if (n >= 0) return n;
    if (would_block()) return 0;

    net_close(net);
    return -1;
}

int net_recv(net_t *net, void *buf, size_t len)
{
    int n;

    if (net->state != NET_OPEN) return -1;

    n = (int)recv((SOCKET_CAST)net->fd, buf, (int)len, 0);
    if (n > 0) return n;
    if (n < 0 && would_block()) return 0;

    net_close(net);
    return -1;
}

void net_close(net_t *net)
{
    if (net->fd != -1) close_socket(net->fd);
    net->fd = -1;
    net->state = NET_CLOSED;
}
