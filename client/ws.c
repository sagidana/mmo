#include "ws.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define MAX_MESSAGE (1024 * 1024)

/* ---------------- sha1 (for Sec-WebSocket-Accept) ---------------- */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    unsigned char block[64];
    size_t block_len;
} sha1_t;

static uint32_t rol(uint32_t v, int bits)
{
    return (v << bits) | (v >> (32 - bits));
}

static void sha1_init(sha1_t *s)
{
    s->h[0] = 0x67452301;
    s->h[1] = 0xEFCDAB89;
    s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476;
    s->h[4] = 0xC3D2E1F0;
    s->len = 0;
    s->block_len = 0;
}

static void sha1_block(sha1_t *s)
{
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, tmp;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)s->block[i * 4] << 24)
             | ((uint32_t)s->block[i * 4 + 1] << 16)
             | ((uint32_t)s->block[i * 4 + 2] << 8)
             | ((uint32_t)s->block[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d;                            k = 0xCA62C1D6; }

        tmp = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = tmp;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_update(sha1_t *s, const unsigned char *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        s->block[s->block_len++] = data[i];
        if (s->block_len == 64) {
            sha1_block(s);
            s->block_len = 0;
        }
    }
    s->len += len * 8;
}

static void sha1_final(sha1_t *s, unsigned char out[20])
{
    uint64_t len = s->len;
    unsigned char pad = 0x80;
    unsigned char zero = 0x00;
    unsigned char len_be[8];
    int i;

    sha1_update(s, &pad, 1);
    while (s->block_len != 56) sha1_update(s, &zero, 1);

    for (i = 0; i < 8; i++) len_be[i] = (unsigned char)(len >> (56 - i * 8));
    sha1_update(s, len_be, 8);

    for (i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s->h[i]);
    }
}

/* ---------------- base64 ---------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char *in, size_t len, char *out)
{
    size_t i;
    size_t o = 0;

    for (i = 0; i + 2 < len; i += 3) {
        out[o++] = B64[in[i] >> 2];
        out[o++] = B64[((in[i] & 0x03) << 4) | (in[i + 1] >> 4)];
        out[o++] = B64[((in[i + 1] & 0x0F) << 2) | (in[i + 2] >> 6)];
        out[o++] = B64[in[i + 2] & 0x3F];
    }
    if (len - i == 1) {
        out[o++] = B64[in[i] >> 2];
        out[o++] = B64[(in[i] & 0x03) << 4];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        out[o++] = B64[in[i] >> 2];
        out[o++] = B64[((in[i] & 0x03) << 4) | (in[i + 1] >> 4)];
        out[o++] = B64[(in[i + 1] & 0x0F) << 2];
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* ---------------- buffers ---------------- */

static int buf_append(unsigned char **buf, size_t *len, size_t *cap,
                      const unsigned char *data, size_t data_len)
{
    unsigned char *grown;
    size_t need = *len + data_len;

    if (need > *cap) {
        size_t new_cap = *cap == 0 ? 4096 : *cap;
        while (new_cap < need) new_cap *= 2;
        grown = realloc(*buf, new_cap);
        if (grown == NULL) return -1;
        *buf = grown;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len = need;
    return 0;
}

static void buf_consume(unsigned char *buf, size_t *len, size_t n)
{
    memmove(buf, buf + n, *len - n);
    *len -= n;
}

/* ---------------- message queue ---------------- */

static void msg_push(ws_t *ws, const unsigned char *data, size_t len)
{
    ws_msg_t *msg = malloc(sizeof(ws_msg_t));

    if (msg == NULL) return;
    msg->text = malloc(len + 1);
    if (msg->text == NULL) {
        free(msg);
        return;
    }
    memcpy(msg->text, data, len);
    msg->text[len] = '\0';
    msg->next = NULL;

    if (ws->msg_tail != NULL) ws->msg_tail->next = msg;
    else ws->msg_head = msg;
    ws->msg_tail = msg;
}

char *ws_recv_text(ws_t *ws)
{
    ws_msg_t *msg = ws->msg_head;
    char *text;

    if (msg == NULL) return NULL;
    ws->msg_head = msg->next;
    if (ws->msg_head == NULL) ws->msg_tail = NULL;
    text = msg->text;
    free(msg);
    return text;
}

/* ---------------- frames ---------------- */

static void mask_key(unsigned char key[4])
{
    FILE *urandom = fopen("/dev/urandom", "rb");

    if (urandom != NULL) {
        if (fread(key, 1, 4, urandom) != 4) {
            key[0] = 0x12; key[1] = 0x34; key[2] = 0x56; key[3] = 0x78;
        }
        fclose(urandom);
        return;
    }
    key[0] = (unsigned char)rand();
    key[1] = (unsigned char)rand();
    key[2] = (unsigned char)rand();
    key[3] = (unsigned char)rand();
}

static int send_frame(ws_t *ws, int opcode, const unsigned char *payload, size_t len)
{
    unsigned char header[14];
    unsigned char key[4];
    unsigned char *masked;
    size_t header_len = 0;
    size_t i;
    int rc;

    header[header_len++] = (unsigned char)(0x80 | opcode);   /* fin + opcode */

    if (len < 126) {
        header[header_len++] = (unsigned char)(0x80 | len);  /* mask bit + len */
    } else if (len < 65536) {
        header[header_len++] = 0x80 | 126;
        header[header_len++] = (unsigned char)(len >> 8);
        header[header_len++] = (unsigned char)(len & 0xFF);
    } else {
        header[header_len++] = 0x80 | 127;
        for (i = 0; i < 8; i++) header[header_len++] = (unsigned char)(len >> (56 - i * 8));
    }

    mask_key(key);
    memcpy(header + header_len, key, 4);
    header_len += 4;

    if (buf_append(&ws->outbuf, &ws->out_len, &ws->out_cap, header, header_len) != 0) return -1;

    masked = malloc(len == 0 ? 1 : len);
    if (masked == NULL) return -1;
    for (i = 0; i < len; i++) masked[i] = payload[i] ^ key[i % 4];
    rc = buf_append(&ws->outbuf, &ws->out_len, &ws->out_cap, masked, len);
    free(masked);
    return rc;
}

int ws_send_text(ws_t *ws, const char *text)
{
    if (ws->state != WS_OPEN && ws->state != WS_HANDSHAKE) return -1;
    return send_frame(ws, 0x1, (const unsigned char *)text, strlen(text));
}

static void frag_append(ws_t *ws, const unsigned char *data, size_t len)
{
    unsigned char *grown = realloc(ws->frag, ws->frag_len + len + 1);

    if (grown == NULL) return;
    ws->frag = grown;
    memcpy(ws->frag + ws->frag_len, data, len);
    ws->frag_len += len;
}

/* parses one frame from inbuf; returns bytes consumed or 0 if incomplete */
static size_t parse_frame(ws_t *ws)
{
    unsigned char *buf = ws->inbuf;
    size_t avail = ws->in_len;
    size_t pos = 2;
    int fin, opcode, masked;
    uint64_t len;
    size_t i;

    if (avail < 2) return 0;

    fin = (buf[0] & 0x80) != 0;
    opcode = buf[0] & 0x0F;
    masked = (buf[1] & 0x80) != 0;
    len = buf[1] & 0x7F;

    if (len == 126) {
        if (avail < pos + 2) return 0;
        len = ((uint64_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;
    } else if (len == 127) {
        if (avail < pos + 8) return 0;
        len = 0;
        for (i = 0; i < 8; i++) len = (len << 8) | buf[pos + i];
        pos += 8;
    }

    if (masked) pos += 4;   /* servers must not mask; skip the key if one does */
    if (len > MAX_MESSAGE) {
        ws_close(ws);
        return 0;
    }
    if (avail < pos + len) return 0;

    if (masked) {
        for (i = 0; i < len; i++) buf[pos + i] ^= buf[pos - 4 + i % 4];
    }

    switch (opcode) {
    case 0x0:   /* continuation */
        frag_append(ws, buf + pos, len);
        if (fin && ws->frag != NULL) {
            msg_push(ws, ws->frag, ws->frag_len);
            free(ws->frag);
            ws->frag = NULL;
            ws->frag_len = 0;
        }
        break;
    case 0x1:   /* text */
    case 0x2:   /* binary (treated as text; layer 1 is json only) */
        if (fin) {
            msg_push(ws, buf + pos, len);
        } else {
            free(ws->frag);
            ws->frag = NULL;
            ws->frag_len = 0;
            frag_append(ws, buf + pos, len);
        }
        break;
    case 0x8:   /* close */
        send_frame(ws, 0x8, buf + pos, len > 125 ? 125 : len);
        ws->state = WS_CLOSED;
        break;
    case 0x9:   /* ping -> pong with same payload */
        send_frame(ws, 0xA, buf + pos, len);
        break;
    case 0xA:   /* pong: ignore */
        break;
    default:
        break;
    }

    return pos + (size_t)len;
}

/* ---------------- handshake + service ---------------- */

int ws_connect(ws_t *ws, const char *host, const char *port)
{
    unsigned char raw_key[16];
    FILE *urandom;
    size_t i;

    memset(ws, 0, sizeof(*ws));

    urandom = fopen("/dev/urandom", "rb");
    if (urandom != NULL) {
        if (fread(raw_key, 1, 16, urandom) != 16) {
            for (i = 0; i < 16; i++) raw_key[i] = (unsigned char)rand();
        }
        fclose(urandom);
    } else {
        for (i = 0; i < 16; i++) raw_key[i] = (unsigned char)rand();
    }
    b64_encode(raw_key, 16, ws->key_b64);

    snprintf(ws->host_header, sizeof(ws->host_header), "%s:%s", host, port);

    if (net_connect(&ws->net, host, port) != 0) {
        ws->state = WS_CLOSED;
        return -1;
    }
    ws->state = WS_CONNECTING;
    return 0;
}

static void send_upgrade_request(ws_t *ws)
{
    char request[512];
    int len;

    len = snprintf(request, sizeof(request),
                   "GET / HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "Upgrade: websocket\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\n"
                   "Sec-WebSocket-Version: 13\r\n"
                   "\r\n",
                   ws->host_header, ws->key_b64);
    buf_append(&ws->outbuf, &ws->out_len, &ws->out_cap,
               (const unsigned char *)request, (size_t)len);
}

static int accept_key_matches(ws_t *ws, const char *headers)
{
    char joined[96];
    unsigned char digest[20];
    char expected[32];
    const char *line;
    sha1_t sha;

    snprintf(joined, sizeof(joined), "%s%s", ws->key_b64, WS_GUID);
    sha1_init(&sha);
    sha1_update(&sha, (const unsigned char *)joined, strlen(joined));
    sha1_final(&sha, digest);
    b64_encode(digest, 20, expected);

    line = strstr(headers, "Sec-WebSocket-Accept:");
    if (line == NULL) line = strstr(headers, "sec-websocket-accept:");
    if (line == NULL) return 0;
    line += strlen("Sec-WebSocket-Accept:");
    while (*line == ' ') line++;

    return strncmp(line, expected, strlen(expected)) == 0;
}

static void service_handshake(ws_t *ws)
{
    char *headers_end;
    size_t header_len;

    /* NUL-terminate the buffer view for strstr */
    if (buf_append(&ws->inbuf, &ws->in_len, &ws->in_cap, (const unsigned char *)"", 1) != 0) return;
    ws->in_len -= 1;
    ws->inbuf[ws->in_len] = '\0';

    headers_end = strstr((char *)ws->inbuf, "\r\n\r\n");
    if (headers_end == NULL) return;
    header_len = (size_t)(headers_end - (char *)ws->inbuf) + 4;

    if (strncmp((char *)ws->inbuf, "HTTP/1.1 101", 12) != 0 || !accept_key_matches(ws, (char *)ws->inbuf)) {
        ws_close(ws);
        return;
    }

    buf_consume(ws->inbuf, &ws->in_len, header_len);
    ws->state = WS_OPEN;
}

void ws_service(ws_t *ws)
{
    unsigned char chunk[8192];
    int n;
    size_t consumed;

    if (ws->state == WS_CLOSED) return;

    if (ws->state == WS_CONNECTING) {
        net_service_connect(&ws->net);
        if (ws->net.state == NET_CLOSED) {
            ws->state = WS_CLOSED;
            return;
        }
        if (ws->net.state == NET_OPEN) {
            send_upgrade_request(ws);
            ws->state = WS_HANDSHAKE;
        }
        return;
    }

    /* flush pending writes */
    while (ws->out_len > 0) {
        n = net_send(&ws->net, ws->outbuf, ws->out_len);
        if (n < 0) {
            ws->state = WS_CLOSED;
            return;
        }
        if (n == 0) break;
        buf_consume(ws->outbuf, &ws->out_len, (size_t)n);
    }

    /* read whatever arrived */
    for (;;) {
        n = net_recv(&ws->net, chunk, sizeof(chunk));
        if (n < 0) {
            ws->state = WS_CLOSED;
            return;
        }
        if (n == 0) break;
        if (buf_append(&ws->inbuf, &ws->in_len, &ws->in_cap, chunk, (size_t)n) != 0) return;
    }

    if (ws->state == WS_HANDSHAKE) {
        service_handshake(ws);
        if (ws->state != WS_OPEN) return;
    }

    while (ws->state == WS_OPEN) {
        consumed = parse_frame(ws);
        if (consumed == 0) break;
        buf_consume(ws->inbuf, &ws->in_len, consumed);
    }
}

void ws_close(ws_t *ws)
{
    char *text;

    net_close(&ws->net);
    ws->state = WS_CLOSED;

    free(ws->inbuf);
    ws->inbuf = NULL;
    ws->in_len = 0;
    ws->in_cap = 0;

    free(ws->outbuf);
    ws->outbuf = NULL;
    ws->out_len = 0;
    ws->out_cap = 0;

    free(ws->frag);
    ws->frag = NULL;
    ws->frag_len = 0;

    for (;;) {
        text = ws_recv_text(ws);
        if (text == NULL) break;
        free(text);
    }
}
