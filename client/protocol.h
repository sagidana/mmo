#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "vendor/cjson/cJSON.h"

#define PROTO_VERSION 1

typedef struct {
    char type[32];
    int seq;                /* -1 when the message is an event */
    cJSON *data;            /* borrowed from root; may be NULL */
    cJSON *root;
} proto_msg_t;

/* builders return a malloc'd json string; caller frees */
char *proto_hello(int seq);
char *proto_auth_password(int seq, const char *name, const char *password);
char *proto_auth_token(int seq, const char *name, const char *token);
char *proto_ping(int seq, double t);
char *proto_intent(int seq, const char *op, const char *motion, int count);

/* parses an envelope; returns 0 on success; call proto_msg_free after */
int proto_parse(const char *raw, proto_msg_t *msg);
void proto_msg_free(proto_msg_t *msg);

/* helpers over msg->data; return fallback when absent */
const char *proto_data_str(proto_msg_t *msg, const char *key, const char *fallback);
double proto_data_num(proto_msg_t *msg, const char *key, double fallback);

#endif
