#include "raylib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ws.h"
#include "protocol.h"

#define MAX_PLAYERS 128
#define MAX_LOG 8
#define MAX_EVENTS 256
#define TOAST_TTL 7.0
#define TOAST_FADE 1.5
#define TOAST_SHOWN 6
#define PING_INTERVAL 10.0
#define MAX_COUNT 4096
#define MAX_TRAIL 256
#define TRAIL_TTL 0.7f
#define TRAIL_STAGGER 0.05f
#define ATTACK_TTL 0.35f
#define DEATH_TTL 0.7f
#define HIT_FLASH 0.3
#define DEATH_EMBERS 16
#define MAX_DMG 32
#define DMG_TTL 0.6f

enum {
    TRAIL_WARM,      /* own movement */
    TRAIL_COLD,      /* others' movement */
    TRAIL_ATTACK,    /* melee swing streak */
    TRAIL_DEATH,     /* death burst ember */
};

enum {
    SCREEN_CONNECT,
    SCREEN_LOGIN,
    SCREEN_WORLD,
};

enum {
    CONN_IDLE,
    CONN_SOCKET,
    CONN_HELLO_SENT,
    CONN_GREETED,
    CONN_AUTH_SENT,
    CONN_AUTHED,
};

static const Color BG = { 18, 18, 18, 255 };
static const Color FG = { 205, 205, 205, 255 };
static const Color DIM = { 110, 110, 110, 255 };
static const Color ACCENT = { 120, 180, 120, 255 };
static const Color ERRCOL = { 190, 110, 110, 255 };

static const Color FLOOR_A = { 38, 38, 42, 255 };
static const Color FLOOR_B = { 44, 44, 48, 255 };
static const Color WALL_COL = { 70, 70, 78, 255 };

typedef struct {
    int used;
    char name[33];
    int x;
    int y;
    float rx;              /* render position, lerped toward x/y */
    float ry;
    float hp;              /* client-simulated between server checkpoints */
    float hp_max;
    float hp_shown;        /* eases toward hp so damage visibly drains */
    double hit_time;
} player_t;

typedef struct {
    int used;
    float wx;
    float wy;
    int amount;
    float born;
} dmg_t;

typedef struct {
    int used;
    int x;
    int y;
    int kind;
    float born;
    float vx;            /* drift direction, death embers only */
    float vy;
} trail_t;

typedef struct {
    int screen;
    int conn;
    ws_t ws;
    int seq;

    char address[128];
    char name[33];
    char password[64];
    int focus;

    char motd[128];
    char error[160];

    /* world */
    int map_w;
    int map_h;
    unsigned char *tiles;
    int have_map;
    int have_state;
    player_t players[MAX_PLAYERS];   /* others, not self */
    trail_t trail[MAX_TRAIL];
    int trail_next;
    dmg_t dmg[MAX_DMG];              /* floating damage numbers */
    int dmg_next;
    int self_x;
    int self_y;
    float self_rx;
    float self_ry;
    int outstanding;                 /* in-flight intents */

    /* vitals: simulated locally at rules rates, snapped by server checkpoints */
    float hp;
    float stamina;
    float hp_max;
    float stamina_max;
    float hp_regen;
    float stamina_regen;
    float hp_shown;                  /* eased display value for own bars */
    float stamina_shown;
    int move_cost;
    int melee_cost;
    int dash_mult;
    double self_hit_time;

    /* input */
    int count;                       /* pending count prefix, 0 = none */
    int pending_z;                   /* saw 'z', waiting for second key */
    int pending_op;                  /* 'd' waiting for its motion, 0 = none */
    int last_kind;                   /* dot-repeat: 0 none, 1 move, 2 melee */
    char last_motion[2];
    int last_count;
    int cmd_open;                    /* vim-style ':' command line */
    char cmd[128];
    int want_quit;

    Camera3D camera;
    float cam_dist;
    float cam_x;               /* fixed view anchor; moves only on dead-zone hit or z ops */
    float cam_y;

    char events[MAX_EVENTS][192];    /* ring buffer of game events */
    double event_born[MAX_EVENTS];
    int event_head;                  /* next write slot */
    int event_total;                 /* lifetime count (>= entries stored) */
    int console_open;                /* ~ toggles the event console */
    int console_scroll;              /* lines scrolled up from the bottom */

    double last_ping;
    double rtt_ms;

    char token[64];
    int player_id;

} app_t;

static void trail_add_path(app_t *app, int x0, int y0, int x1, int y1, int kind);

/* ---------------- log + players ---------------- */

static void log_line(app_t *app, const char *line)
{
    int slot = app->event_head;

    snprintf(app->events[slot], sizeof(app->events[0]), "%s", line);
    app->event_born[slot] = GetTime();
    app->event_head = (app->event_head + 1) % MAX_EVENTS;
    app->event_total++;
}

static player_t *player_find(app_t *app, const char *name)
{
    int i;

    for (i = 0; i < MAX_PLAYERS; i++) {
        if (app->players[i].used && strcmp(app->players[i].name, name) == 0) {
            return &app->players[i];
        }
    }
    return NULL;
}

static void player_upsert(app_t *app, const char *name, int x, int y)
{
    player_t *p = player_find(app, name);
    int i;

    if (p == NULL) {
        for (i = 0; i < MAX_PLAYERS; i++) {
            if (!app->players[i].used) {
                p = &app->players[i];
                break;
            }
        }
        if (p == NULL) return;
        p->used = 1;
        snprintf(p->name, sizeof(p->name), "%s", name);
        p->rx = (float)x;
        p->ry = (float)y;
        p->x = x;
        p->y = y;
        p->hp = 0;
        p->hp_max = 0;
        p->hp_shown = 0;
        p->hit_time = 0;
        return;
    }
    trail_add_path(app, p->x, p->y, x, y, TRAIL_COLD);
    p->x = x;
    p->y = y;
}

static void player_remove(app_t *app, const char *name)
{
    player_t *p = player_find(app, name);

    if (p != NULL) p->used = 0;
}

/* ---------------- fire trail ---------------- */

/* marks the tiles crossed from (x0,y0) toward (x1,y1), excluding the
 * destination; oldest at the origin so the trail dies away backward */
static void trail_add_path(app_t *app, int x0, int y0, int x1, int y1, int kind)
{
    int dx = 0;
    int dy = 0;
    int steps;
    int i;
    float now = (float)GetTime();
    trail_t *cell;

    if (x0 != x1 && y0 != y1) return;   /* teleport/diagonal (e.g. respawn): no trail */

    if (x1 > x0) dx = 1;
    if (x1 < x0) dx = -1;
    if (y1 > y0) dy = 1;
    if (y1 < y0) dy = -1;
    steps = abs(x1 - x0) + abs(y1 - y0);
    if (steps == 0 || steps > 40) return;

    for (i = 0; i < steps; i++) {
        cell = &app->trail[app->trail_next];
        app->trail_next = (app->trail_next + 1) % MAX_TRAIL;
        cell->used = 1;
        cell->x = x0 + dx * i;
        cell->y = y0 + dy * i;
        cell->kind = kind;
        cell->born = now - TRAIL_STAGGER * (float)(steps - 1 - i);
        cell->vx = 0.0f;
        cell->vy = 0.0f;
    }
}

/* death burst: a ring of embers flung outward from a tile */
static void trail_add_burst(app_t *app, float fx, float fy)
{
    float now = (float)GetTime();
    int i;

    for (i = 0; i < DEATH_EMBERS; i++) {
        trail_t *cell = &app->trail[app->trail_next];
        float ang = (float)i * (2.0f * PI / (float)DEATH_EMBERS);

        app->trail_next = (app->trail_next + 1) % MAX_TRAIL;
        cell->used = 1;
        cell->x = (int)fx;      /* base tile; sub-tile drift comes from vx/vy */
        cell->y = (int)fy;
        cell->kind = TRAIL_DEATH;
        cell->born = now;
        cell->vx = cosf(ang) * (0.7f + 0.3f * sinf(ang * 3.0f));
        cell->vy = sinf(ang) * (0.7f + 0.3f * cosf(ang * 3.0f));
    }
}

static void dmg_add(app_t *app, float wx, float wy, int amount)
{
    dmg_t *d;

    if (amount < 1) return;
    d = &app->dmg[app->dmg_next];
    app->dmg_next = (app->dmg_next + 1) % MAX_DMG;
    d->used = 1;
    d->wx = wx;
    d->wy = wy;
    d->amount = amount;
    d->born = (float)GetTime();
}

/* melee swing: streak along the ray, including the far tile */
static void trail_add_attack(app_t *app, int x, int y, const char *motion, int count)
{
    int dx = 0;
    int dy = 0;

    if (motion[0] == 'h') dx = -1;
    if (motion[0] == 'l') dx = 1;
    if (motion[0] == 'k') dy = -1;
    if (motion[0] == 'j') dy = 1;
    if (count > 40) count = 40;
    trail_add_path(app, x + dx, y + dy, x + dx * (count + 1), y + dy * (count + 1), TRAIL_ATTACK);
}

/* ---------------- local world rules (mirror of server/world.py) ---------------- */

static int tile_at(app_t *app, int x, int y)
{
    if (x < 0 || y < 0 || x >= app->map_w || y >= app->map_h) return 1;
    return app->tiles[y * app->map_w + x];
}

static int occupied(app_t *app, int x, int y)
{
    int i;

    for (i = 0; i < MAX_PLAYERS; i++) {
        if (app->players[i].used && app->players[i].x == x && app->players[i].y == y) return 1;
    }
    return 0;
}

static int local_resolve_move(app_t *app, int dx, int dy, int count, int *out_x, int *out_y)
{
    int x = app->self_x;
    int y = app->self_y;
    int granted = 0;

    while (granted < count) {
        int nx = x + dx;
        int ny = y + dy;
        if (tile_at(app, nx, ny) != 0) break;
        if (occupied(app, nx, ny)) break;
        x = nx;
        y = ny;
        granted++;
    }
    *out_x = x;
    *out_y = y;
    return granted;
}

/* ---------------- connection ---------------- */

static void disconnect(app_t *app, const char *why)
{
    int i;

    if (app->ws.state != WS_CLOSED) ws_close(&app->ws);
    app->conn = CONN_IDLE;
    app->screen = SCREEN_CONNECT;
    app->rtt_ms = 0;
    app->have_map = 0;
    app->have_state = 0;
    app->outstanding = 0;
    app->count = 0;
    app->pending_z = 0;
    app->pending_op = 0;
    app->last_kind = 0;
    app->self_hit_time = 0;
    free(app->tiles);
    app->tiles = NULL;
    for (i = 0; i < MAX_PLAYERS; i++) app->players[i].used = 0;
    for (i = 0; i < MAX_TRAIL; i++) app->trail[i].used = 0;
    for (i = 0; i < MAX_DMG; i++) app->dmg[i].used = 0;
    app->event_head = 0;
    app->event_total = 0;
    app->console_open = 0;
    app->console_scroll = 0;
    if (why != NULL) snprintf(app->error, sizeof(app->error), "%s", why);
}

static void send_raw(app_t *app, char *raw)
{
    if (raw == NULL) return;
    ws_send_text(&app->ws, raw);
    free(raw);
}

static void start_connect(app_t *app)
{
    char host[128];
    char port[16];
    const char *colon = strchr(app->address, ':');

    if (app->address[0] == '\0') return;

    if (colon != NULL) {
        snprintf(host, sizeof(host), "%.*s", (int)(colon - app->address), app->address);
        snprintf(port, sizeof(port), "%s", colon + 1);
    } else {
        snprintf(host, sizeof(host), "%s", app->address);
        snprintf(port, sizeof(port), "4000");
    }

    app->error[0] = '\0';
    if (ws_connect(&app->ws, host, port) != 0) {
        snprintf(app->error, sizeof(app->error), "cannot resolve %s", app->address);
        return;
    }
    app->conn = CONN_SOCKET;
}

/* ---------------- message handling ---------------- */

static void maybe_enter_world(app_t *app)
{
    if (app->screen == SCREEN_WORLD) return;
    if (!app->have_map || !app->have_state) return;
    app->screen = SCREEN_WORLD;
    app->self_rx = (float)app->self_x;
    app->self_ry = (float)app->self_y;
    app->cam_x = app->self_rx;
    app->cam_y = app->self_ry;
    app->camera.target.x = app->cam_x;
    app->camera.target.z = app->cam_y;
}

/* how far the player may drift from the view anchor before the screen scrolls */
static float view_margin_x(app_t *app)
{
    float m = app->cam_dist * 0.55f;

    if (m < 2.0f) m = 2.0f;
    return m;
}

static float view_margin_y(app_t *app)
{
    float m = app->cam_dist * 0.35f;

    if (m < 2.0f) m = 2.0f;
    return m;
}

static void handle_map(app_t *app, proto_msg_t *msg)
{
    cJSON *tiles;
    cJSON *entry;
    int i = 0;
    int total;

    app->map_w = (int)proto_data_num(msg, "w", 0);
    app->map_h = (int)proto_data_num(msg, "h", 0);
    total = app->map_w * app->map_h;
    if (total <= 0 || total > 1024 * 1024) return;

    free(app->tiles);
    app->tiles = calloc((size_t)total, 1);
    if (app->tiles == NULL) return;

    tiles = cJSON_GetObjectItem(msg->data, "tiles");
    cJSON_ArrayForEach(entry, tiles) {
        if (i >= total) break;
        app->tiles[i] = (unsigned char)entry->valuedouble;
        i++;
    }
    app->have_map = 1;
    maybe_enter_world(app);
}

static void handle_state(app_t *app, proto_msg_t *msg)
{
    cJSON *full = cJSON_GetObjectItem(msg->data, "full");
    cJSON *players = cJSON_GetObjectItem(msg->data, "players");
    cJSON *gone = cJSON_GetObjectItem(msg->data, "gone");
    cJSON *entry;
    int i;

    if (cJSON_IsTrue(full)) {
        for (i = 0; i < MAX_PLAYERS; i++) app->players[i].used = 0;
    }

    cJSON_ArrayForEach(entry, players) {
        cJSON *name = cJSON_GetObjectItem(entry, "name");
        cJSON *x = cJSON_GetObjectItem(entry, "x");
        cJSON *y = cJSON_GetObjectItem(entry, "y");
        cJSON *hp = cJSON_GetObjectItem(entry, "hp");
        cJSON *hp_max = cJSON_GetObjectItem(entry, "hp_max");
        player_t *p;

        if (!cJSON_IsString(name) || !cJSON_IsNumber(x) || !cJSON_IsNumber(y)) continue;

        if (strcmp(name->valuestring, app->name) == 0) {
            app->self_x = (int)x->valuedouble;
            app->self_y = (int)y->valuedouble;
            continue;
        }
        player_upsert(app, name->valuestring, (int)x->valuedouble, (int)y->valuedouble);
        p = player_find(app, name->valuestring);
        if (p != NULL && cJSON_IsNumber(hp)) {
            float new_hp = (float)hp->valuedouble;

            if (cJSON_IsNumber(hp_max)) p->hp_max = (float)hp_max->valuedouble;
            if (new_hp < p->hp) {
                p->hit_time = GetTime();
                dmg_add(app, p->rx, p->ry, (int)(p->hp - new_hp + 0.5f));
            }
            p->hp = new_hp;
            if (p->hp_shown <= 0.0f) p->hp_shown = new_hp;   /* snap on first sight */
        }
    }

    cJSON_ArrayForEach(entry, gone) {
        if (cJSON_IsString(entry)) player_remove(app, entry->valuestring);
    }

    if (cJSON_IsTrue(full)) {
        app->have_state = 1;
        maybe_enter_world(app);
    }
}

static void handle_message(app_t *app, const char *raw)
{
    proto_msg_t msg;
    char line[192];

    if (proto_parse(raw, &msg) != 0) return;

    if (strcmp(msg.type, "hello_ok") == 0) {
        app->conn = CONN_GREETED;
        app->screen = SCREEN_LOGIN;
        snprintf(app->motd, sizeof(app->motd), "%s", proto_data_str(&msg, "motd", ""));
    } else if (strcmp(msg.type, "welcome") == 0) {
        cJSON *rules;

        app->conn = CONN_AUTHED;
        app->error[0] = '\0';
        app->player_id = (int)proto_data_num(&msg, "player_id", 0);
        snprintf(app->token, sizeof(app->token), "%s", proto_data_str(&msg, "token", ""));

        rules = cJSON_GetObjectItem(msg.data, "rules");
        app->hp_max = 100.0f;
        app->stamina_max = 100.0f;
        app->hp_regen = 1.0f;
        app->stamina_regen = 8.0f;
        app->move_cost = 1;
        app->melee_cost = 2;
        app->dash_mult = 4;
        if (rules != NULL) {
            cJSON *item = cJSON_GetObjectItem(rules, "hp_max");
            if (cJSON_IsNumber(item)) app->hp_max = (float)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "stamina_max");
            if (cJSON_IsNumber(item)) app->stamina_max = (float)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "hp_regen");
            if (cJSON_IsNumber(item)) app->hp_regen = (float)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "stamina_regen");
            if (cJSON_IsNumber(item)) app->stamina_regen = (float)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "move_cost");
            if (cJSON_IsNumber(item)) app->move_cost = (int)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "melee_cost");
            if (cJSON_IsNumber(item)) app->melee_cost = (int)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "dash_mult");
            if (cJSON_IsNumber(item)) app->dash_mult = (int)item->valuedouble;
        }
        app->hp = app->hp_max;
        app->stamina = app->stamina_max;
        app->hp_shown = app->hp_max;
        app->stamina_shown = app->stamina_max;
        snprintf(line, sizeof(line), "you entered the world as %s", app->name);
        log_line(app, line);
    } else if (strcmp(msg.type, "map") == 0) {
        handle_map(app, &msg);
    } else if (strcmp(msg.type, "state") == 0) {
        handle_state(app, &msg);
    } else if (strcmp(msg.type, "intent_ok") == 0) {
        if (app->outstanding > 0) app->outstanding--;
        if (app->outstanding == 0) {
            app->self_x = (int)proto_data_num(&msg, "x", app->self_x);
            app->self_y = (int)proto_data_num(&msg, "y", app->self_y);
        }
        app->hp = (float)proto_data_num(&msg, "hp", app->hp);
        app->stamina = (float)proto_data_num(&msg, "stamina", app->stamina);
    } else if (strcmp(msg.type, "vitals") == 0) {
        float new_hp = (float)proto_data_num(&msg, "hp", app->hp);

        if (new_hp < app->hp) {
            app->self_hit_time = GetTime();
            dmg_add(app, app->self_rx, app->self_ry, (int)(app->hp - new_hp + 0.5f));
        }
        app->hp = new_hp;
        app->stamina = (float)proto_data_num(&msg, "stamina", app->stamina);
        if (msg.data != NULL && cJSON_HasObjectItem(msg.data, "x")) {
            /* respawn: snap position and recenter the viewport */
            app->self_x = (int)proto_data_num(&msg, "x", app->self_x);
            app->self_y = (int)proto_data_num(&msg, "y", app->self_y);
            app->self_rx = (float)app->self_x;
            app->self_ry = (float)app->self_y;
            app->cam_x = app->self_rx;
            app->cam_y = app->self_ry;
            app->outstanding = 0;
        }
    } else if (strcmp(msg.type, "melee") == 0) {
        trail_add_attack(app,
                         (int)proto_data_num(&msg, "x", 0),
                         (int)proto_data_num(&msg, "y", 0),
                         proto_data_str(&msg, "motion", "l"),
                         (int)proto_data_num(&msg, "count", 1));
    } else if (strcmp(msg.type, "died") == 0) {
        const char *victim = proto_data_str(&msg, "name", "?");
        player_t *p = player_find(app, victim);

        if (p != NULL) trail_add_burst(app, p->rx, p->ry);
        else if (strcmp(victim, app->name) == 0) trail_add_burst(app, app->self_rx, app->self_ry);

        snprintf(line, sizeof(line), "%s was slain by %s", victim, proto_data_str(&msg, "by", "?"));
        log_line(app, line);
    } else if (strcmp(msg.type, "pong") == 0) {
        app->rtt_ms = GetTime() * 1000.0 - proto_data_num(&msg, "t", 0);
    } else if (strcmp(msg.type, "player_joined") == 0) {
        snprintf(line, sizeof(line), "+ %s joined", proto_data_str(&msg, "name", "?"));
        log_line(app, line);
    } else if (strcmp(msg.type, "player_left") == 0) {
        snprintf(line, sizeof(line), "- %s left", proto_data_str(&msg, "name", "?"));
        log_line(app, line);
    } else if (strcmp(msg.type, "kicked") == 0) {
        proto_msg_free(&msg);
        disconnect(app, "kicked: logged in elsewhere");
        return;
    } else if (strcmp(msg.type, "error") == 0) {
        snprintf(app->error, sizeof(app->error), "%s",
                 proto_data_str(&msg, "message", "unknown error"));
        if (app->conn == CONN_AUTH_SENT) app->conn = CONN_GREETED;
    }

    proto_msg_free(&msg);
}

static void pump_network(app_t *app)
{
    char *raw;

    if (app->conn == CONN_IDLE) return;

    ws_service(&app->ws);

    if (app->ws.state == WS_CLOSED) {
        if (app->conn == CONN_SOCKET) disconnect(app, "connection failed");
        else disconnect(app, "connection lost");
        return;
    }

    if (app->conn == CONN_SOCKET && app->ws.state == WS_OPEN) {
        app->seq++;
        send_raw(app, proto_hello(app->seq));
        app->conn = CONN_HELLO_SENT;
    }

    for (;;) {
        raw = ws_recv_text(&app->ws);
        if (raw == NULL) break;
        handle_message(app, raw);
        free(raw);
        if (app->conn == CONN_IDLE) return;
    }

    if (app->conn >= CONN_GREETED && GetTime() - app->last_ping > PING_INTERVAL) {
        app->last_ping = GetTime();
        app->seq++;
        send_raw(app, proto_ping(app->seq, GetTime() * 1000.0));
    }
}

/* ---------------- command mode ---------------- */

static void cmd_start(app_t *app)
{
    app->cmd_open = 1;
    app->cmd[0] = '\0';
    app->count = 0;
    app->pending_z = 0;
}

static void run_command(app_t *app)
{
    char line[160];
    char *cmd = app->cmd;
    char *arg = strchr(cmd, ' ');

    if (arg != NULL) {
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;
        if (*arg == '\0') arg = NULL;
    }

    if (strcmp(cmd, "q") == 0) {
        app->want_quit = 1;
    } else if (strcmp(cmd, "disconnect") == 0) {
        disconnect(app, NULL);
    } else if (strcmp(cmd, "connect") == 0) {
        if (arg != NULL) snprintf(app->address, sizeof(app->address), "%s", arg);
        if (app->conn != CONN_IDLE) disconnect(app, NULL);
        start_connect(app);
    } else if (cmd[0] != '\0') {
        snprintf(line, sizeof(line), "not a command: %s", cmd);
        snprintf(app->error, sizeof(app->error), "%s", line);
        if (app->screen == SCREEN_WORLD) log_line(app, line);
    }
}

static void update_command_mode(app_t *app)
{
    int ch;
    size_t len = strlen(app->cmd);

    if (IsKeyPressed(KEY_ESCAPE)) {
        app->cmd_open = 0;
        return;
    }
    if (IsKeyPressed(KEY_ENTER)) {
        app->cmd_open = 0;
        run_command(app);
        return;
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (len == 0) {
            app->cmd_open = 0;    /* backspace on an empty cmdline leaves it, like vim */
            return;
        }
        app->cmd[len - 1] = '\0';
        len--;
    }

    for (;;) {
        ch = GetCharPressed();
        if (ch == 0) break;
        if (ch < 32 || ch > 126) continue;
        if (len + 1 >= sizeof(app->cmd)) continue;
        app->cmd[len++] = (char)ch;
        app->cmd[len] = '\0';
    }
}

/* ---------------- input: connect + login ---------------- */

/* returns 1 if ':' on an empty field opened command mode */
static int text_input(app_t *app, char *buf, size_t cap)
{
    int ch;
    size_t len = strlen(buf);

    for (;;) {
        ch = GetCharPressed();
        if (ch == 0) break;
        if (ch == ':' && len == 0) {
            cmd_start(app);
            return 1;
        }
        if (ch < 32 || ch > 126) continue;
        if (len + 1 >= cap) continue;
        buf[len++] = (char)ch;
        buf[len] = '\0';
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (len > 0) buf[len - 1] = '\0';
    }
    return 0;
}

static void update_connect_screen(app_t *app)
{
    if (app->conn != CONN_IDLE) return;

    if (text_input(app, app->address, sizeof(app->address))) return;
    if (IsKeyPressed(KEY_ENTER)) start_connect(app);
}

static void update_login_screen(app_t *app)
{
    int opened;

    if (IsKeyPressed(KEY_TAB)) app->focus = 1 - app->focus;

    if (app->focus == 0) opened = text_input(app, app->name, sizeof(app->name));
    else opened = text_input(app, app->password, sizeof(app->password));
    if (opened) return;

    if (IsKeyPressed(KEY_ESCAPE)) {
        disconnect(app, NULL);
        return;
    }

    if (IsKeyPressed(KEY_ENTER) && app->conn == CONN_GREETED) {
        if (app->name[0] == '\0') {
            snprintf(app->error, sizeof(app->error), "name is empty");
            return;
        }
        if (app->password[0] == '\0') {
            snprintf(app->error, sizeof(app->error), "password is empty");
            return;
        }
        app->error[0] = '\0';
        app->seq++;
        send_raw(app, proto_auth_password(app->seq, app->name, app->password));
        app->conn = CONN_AUTH_SENT;
    }
}

/* ---------------- input: world ---------------- */

static void record_op(app_t *app, int kind, const char *motion, int count)
{
    app->last_kind = kind;
    app->last_motion[0] = motion[0];
    app->last_motion[1] = '\0';
    app->last_count = count;
}

static void do_move(app_t *app, int dx, int dy, const char *motion, int mult)
{
    int count = app->count;
    int tiles;
    int x, y;
    int granted;

    if (count == 0) count = 1;
    app->count = 0;

    /* optimistic echo: predict with the same rule the server runs.
     * a dash moves `mult` tiles per requested count; the server derives the
     * same multiplier from the (capital) motion char. */
    tiles = count * mult;
    if (app->move_cost > 0) {
        int affordable = (int)(app->stamina / (float)app->move_cost);
        if (tiles > affordable) tiles = affordable;
    }
    if (tiles <= 0) return;

    granted = local_resolve_move(app, dx, dy, tiles, &x, &y);
    if (granted > 0) {
        trail_add_path(app, app->self_x, app->self_y, x, y, TRAIL_WARM);
        app->self_x = x;
        app->self_y = y;
        app->stamina -= (float)(granted * app->move_cost);
        if (app->stamina < 0) app->stamina = 0;
    }

    app->seq++;
    app->outstanding++;
    send_raw(app, proto_intent(app->seq, "move", motion, count));
}

static void do_melee(app_t *app, const char *motion)
{
    int count = app->count;
    int pool;

    if (count == 0) count = 1;
    app->count = 0;
    record_op(app, 2, motion, count);

    pool = count;
    if (app->melee_cost > 0) {
        int affordable = (int)(app->stamina / (float)app->melee_cost);
        if (pool > affordable) pool = affordable;
    }
    if (pool > 0) {
        trail_add_attack(app, app->self_x, app->self_y, motion, pool);
        app->stamina -= (float)(pool * app->melee_cost);
        if (app->stamina < 0) app->stamina = 0;
    }

    app->seq++;
    app->outstanding++;
    send_raw(app, proto_intent(app->seq, "melee", motion, count));
}

static void update_world_screen(app_t *app)
{
    int ch;

    /* console eats all input while open */
    if (app->console_open) {
        int stored = app->event_total < MAX_EVENTS ? app->event_total : MAX_EVENTS;

        for (;;) {
            ch = GetCharPressed();
            if (ch == 0) break;
            if (ch == '`' || ch == '~') app->console_open = 0;
            else if (ch == 'j' && app->console_scroll > 0) app->console_scroll--;
            else if (ch == 'k') app->console_scroll++;
        }
        if (IsKeyPressed(KEY_ESCAPE)) app->console_open = 0;
        if (app->console_scroll > stored - 1) app->console_scroll = stored - 1;
        if (app->console_scroll < 0) app->console_scroll = 0;
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        app->count = 0;
        app->pending_z = 0;
        app->pending_op = 0;
    }

    for (;;) {
        ch = GetCharPressed();
        if (ch == 0) break;

        if (app->pending_op == 'd') {
            app->pending_op = 0;
            switch (ch) {
            case 'h': do_melee(app, "h"); break;
            case 'j': do_melee(app, "j"); break;
            case 'k': do_melee(app, "k"); break;
            case 'l': do_melee(app, "l"); break;
            default: app->count = 0; break;
            }
            continue;
        }

        if (app->pending_z) {
            app->pending_z = 0;
            switch (ch) {
            case 'z':                                   /* center vertically */
                app->cam_y = app->self_ry;
                break;
            case 'Z':                                   /* center both axes */
                app->cam_x = app->self_rx;
                app->cam_y = app->self_ry;
                break;
            case 'h':                                   /* view the area to the left */
                app->cam_x = app->self_rx - view_margin_x(app);
                break;
            case 'l':                                   /* view the area to the right */
                app->cam_x = app->self_rx + view_margin_x(app);
                break;
            case 'k':                                   /* view the area above */
                app->cam_y = app->self_ry - view_margin_y(app);
                break;
            case 'j':                                   /* view the area below */
                app->cam_y = app->self_ry + view_margin_y(app);
                break;
            default:
                break;
            }
            continue;
        }

        if (ch >= '1' && ch <= '9') {
            app->count = app->count * 10 + (ch - '0');
            if (app->count > MAX_COUNT) app->count = MAX_COUNT;
            continue;
        }
        if (ch == '0' && app->count > 0) {
            app->count = app->count * 10;
            if (app->count > MAX_COUNT) app->count = MAX_COUNT;
            continue;
        }

        switch (ch) {
        case 'h': do_move(app, -1, 0, "h", 1); break;
        case 'j': do_move(app, 0, 1, "j", 1); break;
        case 'k': do_move(app, 0, -1, "k", 1); break;
        case 'l': do_move(app, 1, 0, "l", 1); break;
        case 'H': do_move(app, -1, 0, "H", app->dash_mult); break;
        case 'J': do_move(app, 0, 1, "J", app->dash_mult); break;
        case 'K': do_move(app, 0, -1, "K", app->dash_mult); break;
        case 'L': do_move(app, 1, 0, "L", app->dash_mult); break;
        case '`':
        case '~': app->console_open = 1; app->console_scroll = 0; break;
        case 'z': app->pending_z = 1; break;
        case 'd': app->pending_op = 'd'; break;
        case '.':                                          /* repeat last melee */
            if (app->last_kind != 2) { app->count = 0; break; }
            if (app->count > 0) app->last_count = app->count;   /* <count>. overrides */
            app->count = app->last_count;
            do_melee(app, app->last_motion);
            break;
        case ':': cmd_start(app); return;
        case '-':
            app->cam_dist += 2.0f;
            if (app->cam_dist > 40.0f) app->cam_dist = 40.0f;
            break;
        case '+':
        case '=':
            app->cam_dist -= 2.0f;
            if (app->cam_dist < 6.0f) app->cam_dist = 6.0f;
            break;
        default:
            app->count = 0;
            break;
        }
    }
}

/* ---------------- drawing ---------------- */

static void draw_field(int x, int y, const char *label, const char *value,
                       int focused, int masked)
{
    char shown[128];
    size_t i;
    size_t len = strlen(value);
    Color color = focused ? FG : DIM;

    if (masked) {
        for (i = 0; i < len && i < sizeof(shown) - 2; i++) shown[i] = '*';
        shown[i] = '\0';
    } else {
        snprintf(shown, sizeof(shown), "%s", value);
    }

    DrawText(label, x, y, 20, DIM);
    DrawText(shown, x + 130, y, 20, color);
    if (focused) DrawText("_", x + 130 + MeasureText(shown, 20) + 2, y, 20, ACCENT);
}

static void draw_status_bar(app_t *app)
{
    char status[192];
    char count_str[16];
    const char *state = "offline";

    if (app->cmd_open) {
        snprintf(status, sizeof(status), ":%s", app->cmd);
        DrawText(status, 10, GetScreenHeight() - 26, 18, FG);
        DrawText("_", 10 + MeasureText(status, 18) + 2, GetScreenHeight() - 26, 18, ACCENT);
        return;
    }

    if (app->conn == CONN_SOCKET) state = "connecting";
    if (app->conn == CONN_HELLO_SENT) state = "handshake";
    if (app->conn == CONN_GREETED || app->conn == CONN_AUTH_SENT) state = "connected";
    if (app->conn == CONN_AUTHED) state = "online";

    if (app->screen == SCREEN_WORLD) {
        int bx = GetScreenWidth() - 340;
        int by = GetScreenHeight() - 24;

        snprintf(status, sizeof(status), " NORMAL | %d,%d | %.0fms",
                 app->self_x, app->self_y, app->rtt_ms);

        DrawText("hp", bx, by, 16, DIM);
        DrawRectangle(bx + 24, by + 4, 84, 8, (Color){ 50, 50, 50, 255 });
        if (app->hp_max > 0) {
            DrawRectangle(bx + 24, by + 4, (int)(84.0f * app->hp_shown / app->hp_max), 8, ACCENT);
        }
        DrawText("st", bx + 122, by, 16, DIM);
        DrawRectangle(bx + 144, by + 4, 84, 8, (Color){ 50, 50, 50, 255 });
        if (app->stamina_max > 0) {
            DrawRectangle(bx + 144, by + 4, (int)(84.0f * app->stamina_shown / app->stamina_max), 8,
                          (Color){ 110, 140, 190, 255 });
        }
    } else if (app->rtt_ms > 0) {
        snprintf(status, sizeof(status), " %s | %s | %.0fms", state, app->address, app->rtt_ms);
    } else if (app->conn != CONN_IDLE) {
        snprintf(status, sizeof(status), " %s | %s", state, app->address);
    } else {
        snprintf(status, sizeof(status), " %s", state);
    }
    DrawText(status, 10, GetScreenHeight() - 26, 18, DIM);

    if (app->screen == SCREEN_WORLD && (app->count > 0 || app->pending_op != 0)) {
        if (app->count > 0 && app->pending_op != 0) {
            snprintf(count_str, sizeof(count_str), "%dd", app->count);
        } else if (app->pending_op != 0) {
            snprintf(count_str, sizeof(count_str), "d");
        } else {
            snprintf(count_str, sizeof(count_str), "%d", app->count);
        }
        DrawText(count_str, GetScreenWidth() - 80, GetScreenHeight() - 26, 18, ACCENT);
    }
}

static void draw_connect_screen(app_t *app)
{
    DrawText("gridmmo", 40, 40, 32, ACCENT);
    DrawText("layer 2", 40, 76, 18, DIM);

    draw_field(40, 140, "server", app->address, 1, 0);
    DrawText("enter to connect", 40, 180, 18, DIM);

    if (app->conn == CONN_SOCKET || app->conn == CONN_HELLO_SENT) {
        DrawText("connecting...", 40, 220, 20, FG);
    }
    if (app->error[0] != '\0') DrawText(app->error, 40, 250, 20, ERRCOL);
}

static void draw_login_screen(app_t *app)
{
    DrawText("gridmmo", 40, 40, 32, ACCENT);
    if (app->motd[0] != '\0') DrawText(app->motd, 40, 80, 18, DIM);

    draw_field(40, 140, "name", app->name, app->focus == 0, 0);
    draw_field(40, 175, "password", app->password, app->focus == 1, 1);

    DrawText("tab to switch, enter to log in (first login registers)", 40, 220, 18, DIM);

    if (app->conn == CONN_AUTH_SENT) DrawText("logging in...", 40, 260, 20, FG);
    if (app->conn == CONN_AUTHED) DrawText("entering world...", 40, 260, 20, FG);
    if (app->error[0] != '\0') DrawText(app->error, 40, 290, 20, ERRCOL);
}

static float approach(float current, float target, float dt)
{
    float step = 12.0f * dt;

    if (step > 1.0f) step = 1.0f;
    return current + (target - current) * step;
}

static unsigned name_seed(const char *name)
{
    unsigned seed = 5381;

    while (*name != '\0') {
        seed = seed * 31 + (unsigned)*name;
        name++;
    }
    return seed;
}

/* procedural voxel flame: rising, shrinking, flickering cubes */
static void draw_flame(float wx, float wy, unsigned seed, int warm)
{
    static const Color WARM_CORE = { 255, 225, 130, 255 };
    static const Color WARM_MID = { 242, 140, 40, 255 };
    static const Color WARM_OUTER = { 200, 60, 30, 255 };
    static const Color COLD_CORE = { 170, 220, 255, 255 };
    static const Color COLD_MID = { 70, 130, 245, 255 };
    static const Color COLD_OUTER = { 45, 60, 200, 255 };

    float t = (float)GetTime();
    float drift = (float)(seed % 97) * 0.0103f;
    int i;

    for (i = 0; i < 14; i++) {
        float life = fmodf(t * 0.85f + (float)i / 14.0f + drift, 1.0f);
        float shrink = 1.0f - life;
        float ang = t * 2.2f + (float)i * 2.39f + (float)(seed % 61);
        float radius = 0.20f * shrink;
        float flicker = 0.82f + 0.26f * sinf(t * 13.0f + (float)i * 1.7f + drift * 40.0f);
        float size = 0.36f * shrink * flicker;
        Vector3 pos;
        Color color;

        if (size < 0.03f) continue;

        pos.x = wx + cosf(ang) * radius + sinf(t * 3.1f + (float)i) * 0.03f;
        pos.y = 0.18f + life * 1.05f;
        pos.z = wy + sinf(ang) * radius + cosf(t * 2.7f + (float)i) * 0.03f;

        if (life < 0.35f) color = warm ? WARM_CORE : COLD_CORE;
        else if (life < 0.7f) color = warm ? WARM_MID : COLD_MID;
        else color = warm ? WARM_OUTER : COLD_OUTER;

        DrawCube(pos, size, size, size, Fade(color, shrink));
    }

    /* ember base so the occupied tile reads crisply */
    {
        Vector3 base = { wx, 0.1f, wy };
        DrawCube(base, 0.3f, 0.2f, 0.3f, warm ? WARM_OUTER : COLD_OUTER);
    }
}

/* text with a 1px black outline so it reads against any 3D backdrop */
static void draw_text_outlined(const char *text, int x, int y, int size, Color color)
{
    Color shadow = { 0, 0, 0, color.a };

    DrawText(text, x - 1, y, size, shadow);
    DrawText(text, x + 1, y, size, shadow);
    DrawText(text, x, y - 1, size, shadow);
    DrawText(text, x, y + 1, size, shadow);
    DrawText(text, x, y, size, color);
}

static void draw_name_tag(app_t *app, const char *name, float wx, float wy, Color color)
{
    Vector3 above = { wx, 1.35f, wy };
    Vector2 screen = GetWorldToScreen(above, app->camera);
    int width = MeasureText(name, 18);

    draw_text_outlined(name, (int)screen.x - width / 2, (int)screen.y, 18, color);
}

static void draw_world_screen(app_t *app)
{
    float dt = GetFrameTime();
    double now_d = GetTime();
    int x, y, i;

    /* own vitals regenerate locally; server checkpoints snap them straight.
     * others' hp is not simulated (npcs don't heal; players' heals arrive as
     * state deltas), so their bars only move on authoritative updates. */
    app->hp = fminf(app->hp_max, app->hp + app->hp_regen * dt);
    app->stamina = fminf(app->stamina_max, app->stamina + app->stamina_regen * dt);

    /* bars ease toward the true value so a hit visibly drains (draft style) */
    app->hp_shown += (app->hp - app->hp_shown) * fminf(1.0f, 6.0f * dt);
    app->stamina_shown += (app->stamina - app->stamina_shown) * fminf(1.0f, 10.0f * dt);
    for (i = 0; i < MAX_PLAYERS; i++) {
        if (!app->players[i].used) continue;
        app->players[i].hp_shown +=
            (app->players[i].hp - app->players[i].hp_shown) * fminf(1.0f, 6.0f * dt);
    }

    /* smooth logical -> render positions */
    app->self_rx = approach(app->self_rx, (float)app->self_x, dt);
    app->self_ry = approach(app->self_ry, (float)app->self_y, dt);
    for (i = 0; i < MAX_PLAYERS; i++) {
        if (!app->players[i].used) continue;
        app->players[i].rx = approach(app->players[i].rx, (float)app->players[i].x, dt);
        app->players[i].ry = approach(app->players[i].ry, (float)app->players[i].y, dt);
    }

    /* vim-style viewport: the anchor moves only when the player hits the margin */
    {
        float mx = view_margin_x(app);
        float my = view_margin_y(app);

        if (app->self_rx < app->cam_x - mx) app->cam_x = app->self_rx + mx;
        if (app->self_rx > app->cam_x + mx) app->cam_x = app->self_rx - mx;
        if (app->self_ry < app->cam_y - my) app->cam_y = app->self_ry + my;
        if (app->self_ry > app->cam_y + my) app->cam_y = app->self_ry - my;
    }
    app->camera.target.x = approach(app->camera.target.x, app->cam_x, dt);
    app->camera.target.z = approach(app->camera.target.z, app->cam_y, dt);
    app->camera.target.y = 0.0f;
    app->camera.position.x = app->camera.target.x;
    app->camera.position.y = app->cam_dist * 1.25f;
    app->camera.position.z = app->camera.target.z + app->cam_dist * 0.3f;

    BeginMode3D(app->camera);

    for (y = 0; y < app->map_h; y++) {
        for (x = 0; x < app->map_w; x++) {
            Vector3 pos = { (float)x, 0.0f, (float)y };
            if (tile_at(app, x, y) == 0) {
                Color c = ((x + y) % 2 == 0) ? FLOOR_A : FLOOR_B;
                pos.y = -0.05f;
                DrawCube(pos, 1.0f, 0.1f, 1.0f, c);
            } else {
                pos.y = 0.5f;
                DrawCube(pos, 1.0f, 1.0f, 1.0f, WALL_COL);
                DrawCubeWires(pos, 1.0f, 1.0f, 1.0f, BG);
            }
        }
    }

    /* fire trail: shrinking, rising embers on tiles recently crossed */
    {
        float now = (float)GetTime();

        for (i = 0; i < MAX_TRAIL; i++) {
            trail_t *cell = &app->trail[i];
            float life;
            float size;
            Vector3 pos;
            Color color;

            if (!cell->used) continue;
            if (cell->kind == TRAIL_ATTACK) life = (now - cell->born) / ATTACK_TTL;
            else if (cell->kind == TRAIL_DEATH) life = (now - cell->born) / DEATH_TTL;
            else life = (now - cell->born) / TRAIL_TTL;
            if (life >= 1.0f) {
                cell->used = 0;
                continue;
            }
            if (life < 0.0f) life = 0.0f;

            size = 0.32f * (1.0f - life) * (0.85f + 0.2f * sinf(now * 15.0f + (float)i * 2.1f));
            if (cell->kind == TRAIL_ATTACK) size *= 1.5f;
            if (size < 0.03f) continue;

            pos.x = (float)cell->x;
            pos.y = 0.12f + life * 0.3f;
            pos.z = (float)cell->y;

            if (cell->kind == TRAIL_WARM) {
                if (life < 0.4f) color = (Color){ 242, 140, 40, 255 };
                else color = (Color){ 200, 60, 30, 255 };
            } else if (cell->kind == TRAIL_COLD) {
                if (life < 0.4f) color = (Color){ 70, 130, 245, 255 };
                else color = (Color){ 45, 60, 200, 255 };
            } else if (cell->kind == TRAIL_ATTACK) {
                pos.y = 0.3f + life * 0.2f;
                if (life < 0.4f) color = (Color){ 255, 190, 70, 255 };
                else color = (Color){ 255, 80, 30, 255 };
            } else {
                /* death burst: fly outward and arc up, then fade */
                pos.x = (float)cell->x + cell->vx * life * 1.6f;
                pos.z = (float)cell->y + cell->vy * life * 1.6f;
                pos.y = 0.3f + sinf(life * PI) * 0.9f;
                size = 0.30f * (1.0f - life);
                if (life < 0.35f) color = (Color){ 255, 220, 120, 255 };
                else if (life < 0.7f) color = (Color){ 245, 120, 40, 255 };
                else color = (Color){ 150, 40, 25, 255 };
            }
            DrawCube(pos, size, size, size, Fade(color, 0.9f * (1.0f - life)));
        }
    }

    for (i = 0; i < MAX_PLAYERS; i++) {
        if (!app->players[i].used) continue;
        draw_flame(app->players[i].rx, app->players[i].ry, name_seed(app->players[i].name), 0);
        if (now_d - app->players[i].hit_time < HIT_FLASH) {
            float flash = 1.0f - (float)((now_d - app->players[i].hit_time) / HIT_FLASH);
            Vector3 pos = { app->players[i].rx, 0.5f, app->players[i].ry };
            DrawCube(pos, 0.9f, 1.0f, 0.9f, Fade(WHITE, 0.45f * flash));
        }
    }

    draw_flame(app->self_rx, app->self_ry, name_seed(app->name), 1);

    EndMode3D();

    /* health bars in screen space */
    for (i = 0; i < MAX_PLAYERS; i++) {
        player_t *p = &app->players[i];

        if (!p->used) continue;
        if (p->hp_max > 0 && p->hp_shown < p->hp_max - 0.5f) {
            Vector3 above = { p->rx, 1.2f, p->ry };
            Vector2 screen = GetWorldToScreen(above, app->camera);
            float ratio = p->hp_shown / p->hp_max;
            Color fill = ratio > 0.4f ? ACCENT : ERRCOL;

            DrawRectangle((int)screen.x - 15, (int)screen.y - 8, 30, 4, (Color){ 50, 50, 50, 200 });
            DrawRectangle((int)screen.x - 15, (int)screen.y - 8, (int)(30.0f * ratio), 4, fill);
        }
    }

    /* names appear only for the entity under the mouse cursor */
    {
        Vector2 mouse = GetMousePosition();
        const char *hover_name = NULL;
        float hover_rx = 0;
        float hover_ry = 0;
        Color hover_col = ACCENT;
        float best = 40.0f;    /* max hover pick radius in pixels */

        for (i = 0; i < MAX_PLAYERS + 1; i++) {
            const char *name;
            float rx, ry;
            Color col;
            Vector3 center;
            Vector2 screen;
            float d;

            if (i < MAX_PLAYERS) {
                if (!app->players[i].used) continue;
                name = app->players[i].name;
                rx = app->players[i].rx;
                ry = app->players[i].ry;
                col = (Color){ 245, 245, 245, 255 };
            } else {
                name = app->name;
                rx = app->self_rx;
                ry = app->self_ry;
                col = ACCENT;
            }

            center = (Vector3){ rx, 0.5f, ry };
            screen = GetWorldToScreen(center, app->camera);
            d = sqrtf((mouse.x - screen.x) * (mouse.x - screen.x) +
                      (mouse.y - screen.y) * (mouse.y - screen.y));
            if (d < best) {
                best = d;
                hover_name = name;
                hover_rx = rx;
                hover_ry = ry;
                hover_col = col;
            }
        }
        if (hover_name != NULL) draw_name_tag(app, hover_name, hover_rx, hover_ry, hover_col);
    }

    /* floating -N damage numbers rising and fading from the hit */
    for (i = 0; i < MAX_DMG; i++) {
        dmg_t *d = &app->dmg[i];
        float life;
        Vector3 at;
        Vector2 screen;
        char text[16];

        int size;
        int width;
        Color col;

        if (!d->used) continue;
        life = ((float)now_d - d->born) / DMG_TTL;
        if (life >= 1.0f) {
            d->used = 0;
            continue;
        }
        at.x = d->wx;
        at.y = 0.9f + life * 1.6f;    /* rise */
        at.z = d->wy;
        screen = GetWorldToScreen(at, app->camera);
        snprintf(text, sizeof(text), "-%d", d->amount);

        /* pop bigger in the first moment, then settle; fade over the last half */
        size = (life < 0.15f) ? (int)(30 + (0.15f - life) * 160.0f) : 30;
        width = MeasureText(text, size);
        col = (Color){ 255, 90, 70, 255 };
        if (life > 0.5f) col.a = (unsigned char)(255 * (1.0f - (life - 0.5f) / 0.5f));
        draw_text_outlined(text, (int)screen.x - width / 2, (int)screen.y, size, col);
    }

    /* red edge flash when hit */
    if (now_d - app->self_hit_time < HIT_FLASH) {
        float flash = 1.0f - (float)((now_d - app->self_hit_time) / HIT_FLASH);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade((Color){ 200, 40, 30, 255 }, 0.22f * flash));
    }

    /* transient toasts: the newest events, fading out after a few seconds */
    {
        int stored = app->event_total < MAX_EVENTS ? app->event_total : MAX_EVENTS;
        int shown = 0;
        int k;

        for (k = 0; k < stored && shown < TOAST_SHOWN; k++) {
            int slot = ((app->event_head - 1 - k) % MAX_EVENTS + MAX_EVENTS) % MAX_EVENTS;
            double age = now_d - app->event_born[slot];
            float alpha = 1.0f;

            if (age > TOAST_TTL) break;    /* older ones are only older; stop */
            if (age > TOAST_TTL - TOAST_FADE) {
                alpha = (float)((TOAST_TTL - age) / TOAST_FADE);
            }
            draw_text_outlined(app->events[slot], 10, 10 + shown * 20, 16,
                               Fade((Color){ 220, 220, 220, 255 }, alpha));
            shown++;
        }
    }
}

static void draw_console(app_t *app)
{
    int w = GetScreenWidth();
    int h = GetScreenHeight() / 2;
    int stored = app->event_total < MAX_EVENTS ? app->event_total : MAX_EVENTS;
    int rows = (h - 40) / 20;
    int i;

    DrawRectangle(0, 0, w, h, (Color){ 12, 12, 16, 235 });
    DrawRectangle(0, h, w, 2, (Color){ 90, 90, 110, 255 });
    DrawText("console — events (~ to close, j/k to scroll)", 12, 10, 16, DIM);

    /* newest at the bottom, like a terminal; console_scroll pages upward */
    for (i = 0; i < rows && i < stored; i++) {
        int idx = i + app->console_scroll;    /* 0 = newest */
        int slot;
        int y;

        if (idx >= stored) break;
        slot = ((app->event_head - 1 - idx) % MAX_EVENTS + MAX_EVENTS) % MAX_EVENTS;
        y = h - 28 - i * 20;
        DrawText(app->events[slot], 12, y, 16, (Color){ 210, 210, 210, 255 });
    }
}

int main(void)
{
    app_t app;

    srand((unsigned)time(NULL));   /* ws mask/key fallback where /dev/urandom is absent */

    memset(&app, 0, sizeof(app));
    app.screen = SCREEN_CONNECT;
    app.conn = CONN_IDLE;
    snprintf(app.address, sizeof(app.address), "142.132.187.130:4000");

    app.cam_dist = 14.0f;
    app.camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    app.camera.fovy = 55.0f;
    app.camera.projection = CAMERA_PERSPECTIVE;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1100, 700, "gridmmo");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);


    while (!WindowShouldClose() && !app.want_quit) {
        pump_network(&app);

        if (app.cmd_open) {
            update_command_mode(&app);
        } else {
            switch (app.screen) {
            case SCREEN_CONNECT: update_connect_screen(&app); break;
            case SCREEN_LOGIN: update_login_screen(&app); break;
            case SCREEN_WORLD: update_world_screen(&app); break;
            }
        }

        BeginDrawing();
        ClearBackground(BG);
        switch (app.screen) {
        case SCREEN_CONNECT: draw_connect_screen(&app); break;
        case SCREEN_LOGIN: draw_login_screen(&app); break;
        case SCREEN_WORLD: draw_world_screen(&app); break;
        }
        if (app.screen == SCREEN_WORLD && app.console_open) draw_console(&app);
        draw_status_bar(&app);
        EndDrawing();
    }

    if (app.ws.state != WS_CLOSED) ws_close(&app.ws);
    CloseWindow();
    return 0;
}
