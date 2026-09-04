/* headless protocol exerciser: connect, hello, auth, ping, then exit.
 * usage: ./test_cli <host> <port> <name> <password> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ws.h"
#include "protocol.h"

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* returns the next message of the wanted type, skipping events in between */
static char *wait_message(ws_t *ws, const char *wanted, double timeout_ms)
{
    double start = now_ms();
    char *raw;
    proto_msg_t peek;

    while (now_ms() - start < timeout_ms) {
        ws_service(ws);
        if (ws->state == WS_CLOSED) return NULL;
        raw = ws_recv_text(ws);
        if (raw != NULL) {
            if (proto_parse(raw, &peek) != 0) {
                free(raw);
                continue;
            }
            if (strcmp(peek.type, wanted) == 0 || strcmp(peek.type, "error") == 0) {
                proto_msg_free(&peek);
                return raw;
            }
            printf("[.] skipping %s\n", peek.type);
            proto_msg_free(&peek);
            free(raw);
            continue;
        }
        usleep(2000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    ws_t ws;
    char *raw;
    proto_msg_t msg;
    double start;
    double t0;
    int seq = 0;

    if (argc != 5) {
        fprintf(stderr, "usage: %s <host> <port> <name> <password>\n", argv[0]);
        return 1;
    }

    if (ws_connect(&ws, argv[1], argv[2]) != 0) {
        fprintf(stderr, "resolve/connect failed\n");
        return 1;
    }

    start = now_ms();
    while (ws.state != WS_OPEN) {
        ws_service(&ws);
        if (ws.state == WS_CLOSED || now_ms() - start > 3000) {
            fprintf(stderr, "websocket handshake failed\n");
            return 1;
        }
        usleep(2000);
    }
    printf("[+] websocket open\n");

    seq++;
    raw = proto_hello(seq);
    ws_send_text(&ws, raw);
    free(raw);

    raw = wait_message(&ws, "hello_ok", 3000);
    if (raw == NULL || proto_parse(raw, &msg) != 0) {
        fprintf(stderr, "no hello reply\n");
        return 1;
    }
    printf("[+] %s: motd=\"%s\"\n", msg.type, proto_data_str(&msg, "motd", ""));
    if (strcmp(msg.type, "hello_ok") != 0) return 1;
    proto_msg_free(&msg);
    free(raw);

    seq++;
    raw = proto_auth_password(seq, argv[3], argv[4]);
    ws_send_text(&ws, raw);
    free(raw);

    raw = wait_message(&ws, "welcome", 5000);
    if (raw == NULL || proto_parse(raw, &msg) != 0) {
        fprintf(stderr, "no auth reply\n");
        return 1;
    }
    printf("[+] %s: %s\n", msg.type, raw);
    if (strcmp(msg.type, "welcome") != 0) return 1;
    proto_msg_free(&msg);
    free(raw);

    seq++;
    t0 = now_ms();
    raw = proto_ping(seq, t0);
    ws_send_text(&ws, raw);
    free(raw);

    raw = wait_message(&ws, "pong", 3000);
    if (raw == NULL || proto_parse(raw, &msg) != 0) {
        fprintf(stderr, "no pong\n");
        return 1;
    }
    printf("[+] %s: rtt=%.1fms\n", msg.type, now_ms() - proto_data_num(&msg, "t", 0));
    if (strcmp(msg.type, "pong") != 0) return 1;
    proto_msg_free(&msg);
    free(raw);

    ws_close(&ws);
    printf("[+] all good\n");
    return 0;
}
