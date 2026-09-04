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
#define PING_INTERVAL 10.0
#define MAX_COUNT 4096

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
static const Color OTHER_COL = { 170, 150, 110, 255 };

typedef struct {
    int used;
    char name[33];
    int x;
    int y;
    float rx;              /* render position, lerped toward x/y */
    float ry;
} player_t;

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
    int self_x;
    int self_y;
    float self_rx;
    float self_ry;
    int outstanding;                 /* in-flight intents */

    /* input */
    int count;                       /* pending count prefix, 0 = none */
    int pending_z;                   /* saw 'z', waiting for second key */
    int cmd_open;                    /* vim-style ':' command line */
    char cmd[128];
    int want_quit;

    Camera3D camera;
    float cam_dist;
    float cam_x;               /* fixed view anchor; moves only on dead-zone hit or z ops */
    float cam_y;

    char log[MAX_LOG][160];
    int log_count;

    double last_ping;
    double rtt_ms;

    char token[64];
    int player_id;
} app_t;

/* ---------------- log + players ---------------- */

static void log_line(app_t *app, const char *line)
{
    int i;

    if (app->log_count == MAX_LOG) {
        for (i = 1; i < MAX_LOG; i++) strcpy(app->log[i - 1], app->log[i]);
        app->log_count--;
    }
    snprintf(app->log[app->log_count], sizeof(app->log[0]), "%s", line);
    app->log_count++;
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
    }
    p->x = x;
    p->y = y;
}

static void player_remove(app_t *app, const char *name)
{
    player_t *p = player_find(app, name);

    if (p != NULL) p->used = 0;
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
    free(app->tiles);
    app->tiles = NULL;
    for (i = 0; i < MAX_PLAYERS; i++) app->players[i].used = 0;
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
        if (!cJSON_IsString(name) || !cJSON_IsNumber(x) || !cJSON_IsNumber(y)) continue;

        if (strcmp(name->valuestring, app->name) == 0) {
            app->self_x = (int)x->valuedouble;
            app->self_y = (int)y->valuedouble;
        } else {
            player_upsert(app, name->valuestring, (int)x->valuedouble, (int)y->valuedouble);
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
        app->conn = CONN_AUTHED;
        app->error[0] = '\0';
        app->player_id = (int)proto_data_num(&msg, "player_id", 0);
        snprintf(app->token, sizeof(app->token), "%s", proto_data_str(&msg, "token", ""));
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

static void do_move(app_t *app, int dx, int dy, const char *motion)
{
    int count = app->count;
    int x, y;
    int granted;

    if (count == 0) count = 1;
    app->count = 0;

    /* optimistic echo: predict with the same rule the server runs */
    granted = local_resolve_move(app, dx, dy, count, &x, &y);
    if (granted > 0) {
        app->self_x = x;
        app->self_y = y;
    }

    app->seq++;
    app->outstanding++;
    send_raw(app, proto_intent(app->seq, "move", motion, count));
}

static void update_world_screen(app_t *app)
{
    int ch;

    if (IsKeyPressed(KEY_ESCAPE)) {
        app->count = 0;
        app->pending_z = 0;
    }

    for (;;) {
        ch = GetCharPressed();
        if (ch == 0) break;

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
        case 'h': do_move(app, -1, 0, "h"); break;
        case 'j': do_move(app, 0, 1, "j"); break;
        case 'k': do_move(app, 0, -1, "k"); break;
        case 'l': do_move(app, 1, 0, "l"); break;
        case 'z': app->pending_z = 1; break;
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
        snprintf(status, sizeof(status), " NORMAL | %d,%d | %.0fms",
                 app->self_x, app->self_y, app->rtt_ms);
    } else if (app->rtt_ms > 0) {
        snprintf(status, sizeof(status), " %s | %s | %.0fms", state, app->address, app->rtt_ms);
    } else if (app->conn != CONN_IDLE) {
        snprintf(status, sizeof(status), " %s | %s", state, app->address);
    } else {
        snprintf(status, sizeof(status), " %s", state);
    }
    DrawText(status, 10, GetScreenHeight() - 26, 18, DIM);

    if (app->screen == SCREEN_WORLD && app->count > 0) {
        snprintf(count_str, sizeof(count_str), "%d", app->count);
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

static void draw_name_tag(app_t *app, const char *name, float wx, float wy, Color color)
{
    Vector3 above = { wx, 1.2f, wy };
    Vector2 screen = GetWorldToScreen(above, app->camera);
    int width = MeasureText(name, 16);

    DrawText(name, (int)screen.x - width / 2, (int)screen.y, 16, color);
}

static void draw_world_screen(app_t *app)
{
    float dt = GetFrameTime();
    int x, y, i;

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
    app->camera.position.y = app->cam_dist * 1.1f;
    app->camera.position.z = app->camera.target.z + app->cam_dist * 0.7f;

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

    for (i = 0; i < MAX_PLAYERS; i++) {
        if (!app->players[i].used) continue;
        Vector3 pos = { app->players[i].rx, 0.35f, app->players[i].ry };
        DrawCube(pos, 0.7f, 0.7f, 0.7f, OTHER_COL);
    }

    {
        Vector3 pos = { app->self_rx, 0.35f, app->self_ry };
        DrawCube(pos, 0.7f, 0.7f, 0.7f, ACCENT);
    }

    EndMode3D();

    /* name tags in screen space */
    for (i = 0; i < MAX_PLAYERS; i++) {
        if (!app->players[i].used) continue;
        draw_name_tag(app, app->players[i].name, app->players[i].rx, app->players[i].ry, FG);
    }
    draw_name_tag(app, app->name, app->self_rx, app->self_ry, ACCENT);

    for (i = 0; i < app->log_count; i++) {
        DrawText(app->log[i], 10, 10 + i * 20, 16, DIM);
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
        draw_status_bar(&app);
        EndDrawing();
    }

    if (app.ws.state != WS_CLOSED) ws_close(&app.ws);
    CloseWindow();
    return 0;
}
