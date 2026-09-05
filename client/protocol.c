#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *envelope(const char *type, int seq, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    char *raw;

    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "seq", seq);
    cJSON_AddItemToObject(root, "data", data);
    raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return raw;
}

char *proto_hello(int seq)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddNumberToObject(data, "proto", PROTO_VERSION);
    cJSON_AddStringToObject(data, "client", "gridmmo/0.1");
    return envelope("hello", seq, data);
}

char *proto_auth_password(int seq, const char *name, const char *password)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "name", name);
    cJSON_AddStringToObject(data, "password", password);
    return envelope("auth", seq, data);
}

char *proto_auth_token(int seq, const char *name, const char *token)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "name", name);
    cJSON_AddStringToObject(data, "token", token);
    return envelope("auth", seq, data);
}

char *proto_ping(int seq, double t)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddNumberToObject(data, "t", t);
    return envelope("ping", seq, data);
}

char *proto_intent(int seq, const char *op, const char *motion, int count)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "op", op);
    cJSON_AddStringToObject(data, "motion", motion);
    cJSON_AddNumberToObject(data, "count", count);
    return envelope("intent", seq, data);
}

char *proto_retry(int seq)
{
    return envelope("retry", seq, cJSON_CreateObject());
}

char *proto_spell(int seq, const char *spell)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "spell", spell);
    return envelope("spell", seq, data);
}

char *proto_repeat(int seq, int count)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "op", "repeat");
    if (count > 0) cJSON_AddNumberToObject(data, "count", count);
    return envelope("intent", seq, data);
}

char *proto_intent_load(int seq, const char *op, const char *motion, int count)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "op", op);
    cJSON_AddStringToObject(data, "motion", motion);
    cJSON_AddNumberToObject(data, "count", count);
    cJSON_AddBoolToObject(data, "load", 1);
    return envelope("intent", seq, data);
}

char *proto_intent_tile(int seq, int count, int tx, int ty)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "op", "magic");
    cJSON_AddNumberToObject(data, "count", count);
    cJSON_AddNumberToObject(data, "tx", tx);
    cJSON_AddNumberToObject(data, "ty", ty);
    return envelope("intent", seq, data);
}

int proto_parse(const char *raw, proto_msg_t *msg)
{
    cJSON *root = cJSON_Parse(raw);
    cJSON *type;
    cJSON *seq;

    memset(msg, 0, sizeof(*msg));
    if (root == NULL) return -1;

    type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return -1;
    }

    msg->root = root;
    snprintf(msg->type, sizeof(msg->type), "%s", type->valuestring);

    seq = cJSON_GetObjectItem(root, "seq");
    if (cJSON_IsNumber(seq)) msg->seq = (int)seq->valuedouble;
    else msg->seq = -1;

    msg->data = cJSON_GetObjectItem(root, "data");
    return 0;
}

void proto_msg_free(proto_msg_t *msg)
{
    if (msg->root != NULL) cJSON_Delete(msg->root);
    msg->root = NULL;
    msg->data = NULL;
}

const char *proto_data_str(proto_msg_t *msg, const char *key, const char *fallback)
{
    cJSON *item;

    if (msg->data == NULL) return fallback;
    item = cJSON_GetObjectItem(msg->data, key);
    if (!cJSON_IsString(item)) return fallback;
    return item->valuestring;
}

double proto_data_num(proto_msg_t *msg, const char *key, double fallback)
{
    cJSON *item;

    if (msg->data == NULL) return fallback;
    item = cJSON_GetObjectItem(msg->data, key);
    if (!cJSON_IsNumber(item)) return fallback;
    return item->valuedouble;
}
