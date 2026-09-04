#ifndef WS_H
#define WS_H

#include "net.h"

enum {
    WS_CONNECTING,   /* tcp connect in progress */
    WS_HANDSHAKE,    /* http upgrade sent, waiting for 101 */
    WS_OPEN,
    WS_CLOSED,
};

typedef struct ws_msg {
    char *text;
    struct ws_msg *next;
} ws_msg_t;

typedef struct {
    net_t net;
    int state;
    char key_b64[32];              /* Sec-WebSocket-Key sent in the upgrade */
    char host_header[256];

    unsigned char *inbuf;
    size_t in_len;
    size_t in_cap;

    unsigned char *outbuf;
    size_t out_len;
    size_t out_cap;

    unsigned char *frag;           /* continuation-frame accumulator */
    size_t frag_len;

    ws_msg_t *msg_head;            /* complete text messages, fifo */
    ws_msg_t *msg_tail;
} ws_t;

/* starts connecting; path is e.g. "/" */
int ws_connect(ws_t *ws, const char *host, const char *port);

/* pumps the connection: progresses connect/handshake, flushes writes,
 * reads frames; call once per frame */
void ws_service(ws_t *ws);

/* queues one text message for sending; returns -1 if not usable */
int ws_send_text(ws_t *ws, const char *text);

/* returns a malloc'd complete text message or NULL; caller frees */
char *ws_recv_text(ws_t *ws);

void ws_close(ws_t *ws);

#endif
