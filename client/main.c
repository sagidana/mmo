#include "raylib.h"
#include "rlgl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ws.h"
#include "protocol.h"
#include "sound.h"
#include "figures.h"

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
#define MAX_ATTACK_FX 32
#define ATTACK_FX_TTL 0.3f
#define MAX_TELEGRAPH 24
#define MAX_SPELLS 8
#define MAX_BOLTS 16

enum {
    TRAIL_WARM,      /* own movement */
    TRAIL_COLD,      /* others' movement */
    TRAIL_ATTACK,    /* melee swing streak */
    TRAIL_DEATH,     /* death burst ember */
};

enum {
    SCREEN_SETUP,    /* first boot: pick name + password, saved to gridmmo.cfg */
    SCREEN_HOME,     /* empty vim-style splash; :connect to play */
    SCREEN_WORLD,
};

#define MAX_HISTORY 32
#define CONFIG_FILE "gridmmo.cfg"
#define HISTORY_FILE "gridmmo.hist"
#define DEFAULT_ADDRESS "142.132.187.130:4000"

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

#include "font_data.h"

static Font PX_FONT;
static int PX_OK = 0;

/* bitmap pixel fonts only look right at integer scales: snap the requested
 * size to a multiple of the font's base size */
static float px_scale(int size)
{
    float scale = (float)size / (float)PX_FONT.baseSize;

    if (scale < 1.0f) return 1.0f;
    return floorf(scale + 0.3f);
}

static void px_text(const char *text, int x, int y, int size, Color color)
{
    float scale;

    if (!PX_OK) {
        DrawText(text, x, y, size, color);
        return;
    }
    scale = px_scale(size);
    DrawTextEx(PX_FONT, text, (Vector2){ (float)x, (float)y },
               (float)PX_FONT.baseSize * scale, scale, color);
}

static int px_measure(const char *text, int size)
{
    float scale;

    if (!PX_OK) return MeasureText(text, size);
    scale = px_scale(size);
    return (int)MeasureTextEx(PX_FONT, text, (float)PX_FONT.baseSize * scale, scale).x;
}

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
    int defending;
    int figure;
    char facing;
    double moved_at;
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
    int x;             /* attacker tile */
    int y;
    int dx;            /* ray direction */
    int dy;
    int count;         /* ray length in tiles */
    float born;
    int own;           /* gold for own swings, red for incoming */
} attack_fx_t;

typedef struct {
    int used;
    int x;             /* attacker tile */
    int y;
    int dx;
    int dy;
    int count;
    double until;      /* the strike lands here; marker dies then */
    int own;
} telegraph_t;

typedef struct {
    char name[24];
    float cost;
    float speed;
    float range;
    float windup;
    float recover;
} spell_t;

typedef struct {
    int used;
    int x;             /* launch tile */
    int y;
    int dx;
    int dy;
    float speed;
    float range;
    double born;
    int own;
} bolt_t;

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
    attack_fx_t attack_fx[MAX_ATTACK_FX];
    int attack_fx_next;
    telegraph_t telegraph[MAX_TELEGRAPH];
    int telegraph_next;
    bolt_t bolts[MAX_BOLTS];
    int bolt_next;
    spell_t spells[MAX_SPELLS];
    int spell_count;
    int active_spell;                /* index; server confirms via spell_ok */
    int book_open;                   /* the spellbook overlay */
    int book_sel;
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
    float defend_drain;
    float melee_windup;
    float windup_max;
    float melee_recover;
    float recover_max;
    double self_windup_until;        /* own cast: locked until this */
    double self_recover_until;       /* then recovering until this */
    double self_windup_len;
    char self_facing;
    double self_moved_at;
    int defending;
    int defend_off_wanted;           /* we asked to lower the guard (vs. it broke) */
    double self_hit_time;

    /* input */
    int count;                       /* pending count prefix, 0 = none */
    int pending_z;                   /* saw 'z', waiting for second key */
    int pending_op;                  /* 'd' waiting for its motion, 0 = none */
    int last_kind;                   /* dot-repeat: 0 none, 2 melee, 3 magic */
    char last_motion[2];
    int last_count;
    char cmd[128];                   /* console prompt line */
    char cmd_history[MAX_HISTORY][128];
    int hist_count;
    int hist_nav;                    /* -1 = typing live, else index back in history */
    char cmd_stash[128];             /* the live line while browsing history */
    char comp_base[32];              /* tab completion: typed prefix + cycle index */
    int comp_idx;
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

    /* challenge mode */
    int challenge_active;
    char challenge_status[8];        /* run / won / lost */
    char challenge_objective[64];
    double challenge_start;
    float challenge_time;

    double last_ping;
    double rtt_ms;

    char token[64];
    int player_id;

} app_t;

static void trail_add_path(app_t *app, int x0, int y0, int x1, int y1, int kind);
static int occupied(app_t *app, int x, int y);
static int tile_at(app_t *app, int x, int y);
static int self_busy(app_t *app);

/* ---------------- log + players ---------------- */

static void log_line(app_t *app, const char *line)
{
    int slot = app->event_head;

    snprintf(app->events[slot], sizeof(app->events[0]), "%s", line);
    app->event_born[slot] = GetTime();
    app->event_head = (app->event_head + 1) % MAX_EVENTS;
    app->event_total++;
}

/* three lines: name, password, last address; lives next to the binary */
static int config_load(app_t *app)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    char name[64] = "";
    char password[64] = "";
    char address[128] = "";

    if (f == NULL) return 0;
    if (fgets(name, sizeof(name), f) == NULL) name[0] = '\0';
    if (fgets(password, sizeof(password), f) == NULL) password[0] = '\0';
    if (fgets(address, sizeof(address), f) == NULL) address[0] = '\0';
    fclose(f);

    name[strcspn(name, "\r\n")] = '\0';
    password[strcspn(password, "\r\n")] = '\0';
    address[strcspn(address, "\r\n")] = '\0';
    if (name[0] == '\0' || password[0] == '\0') return 0;

    snprintf(app->name, sizeof(app->name), "%s", name);
    snprintf(app->password, sizeof(app->password), "%s", password);
    if (address[0] != '\0') snprintf(app->address, sizeof(app->address), "%s", address);
    return 1;
}

static void config_save(app_t *app)
{
    FILE *f = fopen(CONFIG_FILE, "w");

    if (f == NULL) return;
    fprintf(f, "%s\n%s\n%s\n", app->name, app->password, app->address);
    fclose(f);
}

/* command history survives restarts, viminfo-style */
static void history_load(app_t *app)
{
    FILE *f = fopen(HISTORY_FILE, "r");
    char line[128];

    if (f == NULL) return;
    while (fgets(line, sizeof(line), f) != NULL && app->hist_count < MAX_HISTORY) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        snprintf(app->cmd_history[app->hist_count], sizeof(app->cmd_history[0]), "%s", line);
        app->hist_count++;
    }
    fclose(f);
}

static void history_save(app_t *app)
{
    FILE *f = fopen(HISTORY_FILE, "w");
    int i;

    if (f == NULL) return;
    for (i = 0; i < app->hist_count; i++) fprintf(f, "%s\n", app->cmd_history[i]);
    fclose(f);
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
        p->defending = 0;
        p->figure = FIG_HUMAN;
        p->facing = 'j';
        p->moved_at = 0;
        return;
    }
    trail_add_path(app, p->x, p->y, x, y, TRAIL_COLD);
    if (x != p->x || y != p->y) {
        int dx = x - p->x;
        int dy = y - p->y;

        if (dx > 0) p->facing = 'l';
        if (dx < 0) p->facing = 'h';
        if (dy > 0 && abs(dy) >= abs(dx)) p->facing = 'j';
        if (dy < 0 && abs(dy) >= abs(dx)) p->facing = 'k';
        p->moved_at = GetTime();
    }
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
    sound_play(SND_HIT);
}

/* melee swing: ember streak + a bright blade-cross on every ray tile */
static void trail_add_attack(app_t *app, int x, int y, const char *motion, int count, int own)
{
    int dx = 0;
    int dy = 0;
    attack_fx_t *fx;

    int i;

    if (motion[0] == 'h') dx = -1;
    if (motion[0] == 'l') dx = 1;
    if (motion[0] == 'k') dy = -1;
    if (motion[0] == 'j') dy = 1;
    if (count > 40) count = 40;

    /* the strike stops at walls and at the first body; so does the visual */
    for (i = 1; i <= count; i++) {
        int tx = x + dx * i;
        int ty = y + dy * i;

        if (tile_at(app, tx, ty) != 0) {
            count = i - 1;
            break;
        }
        if (occupied(app, tx, ty) || (tx == app->self_x && ty == app->self_y)) {
            count = i;
            break;
        }
    }
    if (count < 1) return;

    trail_add_path(app, x + dx, y + dy, x + dx * (count + 1), y + dy * (count + 1), TRAIL_ATTACK);

    fx = &app->attack_fx[app->attack_fx_next];
    app->attack_fx_next = (app->attack_fx_next + 1) % MAX_ATTACK_FX;
    fx->used = 1;
    fx->x = x;
    fx->y = y;
    fx->dx = dx;
    fx->dy = dy;
    fx->count = count;
    fx->born = (float)GetTime();
    fx->own = own;
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
    app->screen = SCREEN_HOME;
    app->rtt_ms = 0;
    app->have_map = 0;
    app->have_state = 0;
    app->outstanding = 0;
    app->count = 0;
    app->pending_z = 0;
    app->pending_op = 0;
    app->last_kind = 0;
    app->defending = 0;
    app->defend_off_wanted = 0;
    app->challenge_active = 0;
    app->self_hit_time = 0;
    free(app->tiles);
    app->tiles = NULL;
    for (i = 0; i < MAX_PLAYERS; i++) app->players[i].used = 0;
    for (i = 0; i < MAX_TRAIL; i++) app->trail[i].used = 0;
    for (i = 0; i < MAX_DMG; i++) app->dmg[i].used = 0;
    for (i = 0; i < MAX_ATTACK_FX; i++) app->attack_fx[i].used = 0;
    for (i = 0; i < MAX_TELEGRAPH; i++) app->telegraph[i].used = 0;
    for (i = 0; i < MAX_BOLTS; i++) app->bolts[i].used = 0;
    app->book_open = 0;
    app->self_windup_until = 0;
    app->self_recover_until = 0;
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
        if (p != NULL) {
            cJSON *def = cJSON_GetObjectItem(entry, "def");
            cJSON *fig = cJSON_GetObjectItem(entry, "figure");
            if (cJSON_IsNumber(def)) p->defending = (int)def->valuedouble;
            if (cJSON_IsString(fig)) p->figure = figures_from_name(fig->valuestring);
        }
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
        snprintf(app->motd, sizeof(app->motd), "%s", proto_data_str(&msg, "motd", ""));
        /* log in right away with the saved identity */
        app->seq++;
        send_raw(app, proto_auth_password(app->seq, app->name, app->password));
        app->conn = CONN_AUTH_SENT;
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
        app->defend_drain = 6.0f;
        app->melee_windup = 0.03f;
        app->windup_max = 1.0f;
        app->melee_recover = 0.02f;
        app->recover_max = 0.6f;
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
            item = cJSON_GetObjectItem(rules, "defend_drain");
            if (cJSON_IsNumber(item)) app->defend_drain = (float)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "melee_windup");
            if (cJSON_IsNumber(item)) app->melee_windup = (float)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "windup_max");
            if (cJSON_IsNumber(item)) app->windup_max = (float)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "melee_recover");
            if (cJSON_IsNumber(item)) app->melee_recover = (float)item->valuedouble;
            item = cJSON_GetObjectItem(rules, "recover_max");
            if (cJSON_IsNumber(item)) app->recover_max = (float)item->valuedouble;
        }
        app->hp = app->hp_max;
        app->stamina = app->stamina_max;
        app->hp_shown = app->hp_max;
        app->stamina_shown = app->stamina_max;
        app->spell_count = 0;
        app->active_spell = 0;
        {
            cJSON *spells = cJSON_GetObjectItem(msg.data, "spells");
            cJSON *entry;
            const char *active = proto_data_str(&msg, "active_spell", "");

            cJSON_ArrayForEach(entry, spells) {
                cJSON *sname = cJSON_GetObjectItem(entry, "name");
                spell_t *sp;

                if (!cJSON_IsString(sname)) continue;
                if (app->spell_count >= MAX_SPELLS) break;
                sp = &app->spells[app->spell_count];
                snprintf(sp->name, sizeof(sp->name), "%s", sname->valuestring);
                sp->cost = 3.0f;
                sp->speed = 9.0f;
                sp->range = 15.0f;
                sp->windup = 0.04f;
                sp->recover = 0.02f;
                sname = cJSON_GetObjectItem(entry, "cost");
                if (cJSON_IsNumber(sname)) sp->cost = (float)sname->valuedouble;
                sname = cJSON_GetObjectItem(entry, "speed");
                if (cJSON_IsNumber(sname)) sp->speed = (float)sname->valuedouble;
                sname = cJSON_GetObjectItem(entry, "range");
                if (cJSON_IsNumber(sname)) sp->range = (float)sname->valuedouble;
                sname = cJSON_GetObjectItem(entry, "windup");
                if (cJSON_IsNumber(sname)) sp->windup = (float)sname->valuedouble;
                sname = cJSON_GetObjectItem(entry, "recover");
                if (cJSON_IsNumber(sname)) sp->recover = (float)sname->valuedouble;
                if (strcmp(sp->name, active) == 0) app->active_spell = app->spell_count;
                app->spell_count++;
            }
        }
        config_save(app);    /* remember the address that worked */
        snprintf(line, sizeof(line), "you entered the world as %s", app->name);
        log_line(app, line);
        if (app->motd[0] != '\0') log_line(app, app->motd);
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
        const char *by = proto_data_str(&msg, "by", "?");
        const char *mo = proto_data_str(&msg, "motion", "l");
        int own = strcmp(by, app->name) == 0;
        player_t *pb = player_find(app, by);

        if (pb != NULL) pb->facing = mo[0];
        sound_play(SND_SWING);
        trail_add_attack(app,
                         (int)proto_data_num(&msg, "x", 0),
                         (int)proto_data_num(&msg, "y", 0),
                         mo, (int)proto_data_num(&msg, "count", 1), own);
    } else if (strcmp(msg.type, "windup") == 0) {
        const char *by = proto_data_str(&msg, "by", "?");
        const char *mo = proto_data_str(&msg, "motion", "l");
        telegraph_t *tg = &app->telegraph[app->telegraph_next];
        player_t *pb = player_find(app, by);

        if (pb != NULL) pb->facing = mo[0];
        app->telegraph_next = (app->telegraph_next + 1) % MAX_TELEGRAPH;
        tg->used = 1;
        tg->x = (int)proto_data_num(&msg, "x", 0);
        tg->y = (int)proto_data_num(&msg, "y", 0);
        tg->dx = 0;
        tg->dy = 0;
        if (mo[0] == 'h') tg->dx = -1;
        if (mo[0] == 'l') tg->dx = 1;
        if (mo[0] == 'k') tg->dy = -1;
        if (mo[0] == 'j') tg->dy = 1;
        tg->count = (int)proto_data_num(&msg, "count", 1);
        if (tg->count > 40) tg->count = 40;
        {
            int k;

            for (k = 1; k <= tg->count; k++) {
                if (tile_at(app, tg->x + tg->dx * k, tg->y + tg->dy * k) != 0) {
                    tg->count = k - 1;
                    break;
                }
            }
        }
        tg->until = GetTime() + proto_data_num(&msg, "strike_in", 0.3);
        tg->own = strcmp(by, app->name) == 0;
    } else if (strcmp(msg.type, "defend") == 0) {
        const char *who = proto_data_str(&msg, "name", "?");
        int on = 0;
        cJSON *flag = NULL;

        if (msg.data != NULL) flag = cJSON_GetObjectItem(msg.data, "on");
        if (cJSON_IsTrue(flag)) on = 1;

        if (strcmp(who, app->name) == 0) {
            if (!on && app->defending && !app->defend_off_wanted) {
                log_line(app, "your guard broke");
                sound_play(SND_GUARD_BREAK);
            }
            app->defending = on;
            if (!on) app->defend_off_wanted = 0;
        } else {
            player_t *p = player_find(app, who);
            if (p != NULL) {
                if (p->defending && !on) sound_play(SND_GUARD_OFF);
                if (!p->defending && on) sound_play(SND_GUARD_ON);
                p->defending = on;
            }
        }
    } else if (strcmp(msg.type, "died") == 0) {
        const char *victim = proto_data_str(&msg, "name", "?");
        player_t *p = player_find(app, victim);

        if (p != NULL) trail_add_burst(app, p->rx, p->ry);
        else if (strcmp(victim, app->name) == 0) trail_add_burst(app, app->self_rx, app->self_ry);

        if (strcmp(victim, app->name) == 0) sound_play(SND_DEATH);
        else sound_play(SND_KILL);

        snprintf(line, sizeof(line), "%s was slain by %s", victim, proto_data_str(&msg, "by", "?"));
        log_line(app, line);
    } else if (strcmp(msg.type, "challenge") == 0) {
        const char *status = proto_data_str(&msg, "status", "");

        if (strcmp(status, "start") == 0) {
            app->challenge_active = 1;
            snprintf(app->challenge_status, sizeof(app->challenge_status), "run");
            snprintf(app->challenge_objective, sizeof(app->challenge_objective), "%s",
                     proto_data_str(&msg, "objective", ""));
            app->challenge_start = GetTime();
            snprintf(line, sizeof(line), "challenge: %s", app->challenge_objective);
            log_line(app, line);
        } else if (strcmp(status, "won") == 0) {
            snprintf(app->challenge_status, sizeof(app->challenge_status), "won");
            app->challenge_time = (float)proto_data_num(&msg, "time", 0);
            snprintf(line, sizeof(line), "challenge cleared in %.1fs", app->challenge_time);
            log_line(app, line);
            sound_play(SND_GUARD_ON);
        } else if (strcmp(status, "lost") == 0) {
            snprintf(app->challenge_status, sizeof(app->challenge_status), "lost");
            app->challenge_time = (float)proto_data_num(&msg, "time", 0);
            snprintf(line, sizeof(line), "challenge failed after %.1fs", app->challenge_time);
            log_line(app, line);
        }
    } else if (strcmp(msg.type, "face") == 0) {
        player_t *pf = player_find(app, proto_data_str(&msg, "name", "?"));

        if (pf != NULL) pf->facing = proto_data_str(&msg, "motion", "j")[0];
    } else if (strcmp(msg.type, "spell_ok") == 0) {
        const char *sname = proto_data_str(&msg, "spell", "");
        int i2;

        for (i2 = 0; i2 < app->spell_count; i2++) {
            if (strcmp(app->spells[i2].name, sname) == 0) app->active_spell = i2;
        }
        snprintf(line, sizeof(line), "spell: %s", sname);
        log_line(app, line);
    } else if (strcmp(msg.type, "bolt") == 0) {
        const char *by = proto_data_str(&msg, "by", "?");
        const char *mo = proto_data_str(&msg, "motion", "l");
        bolt_t *b = &app->bolts[app->bolt_next];

        app->bolt_next = (app->bolt_next + 1) % MAX_BOLTS;
        b->used = 1;
        b->x = (int)proto_data_num(&msg, "x", 0);
        b->y = (int)proto_data_num(&msg, "y", 0);
        b->dx = 0;
        b->dy = 0;
        if (mo[0] == 'h') b->dx = -1;
        if (mo[0] == 'l') b->dx = 1;
        if (mo[0] == 'k') b->dy = -1;
        if (mo[0] == 'j') b->dy = 1;
        b->speed = (float)proto_data_num(&msg, "speed", 9);
        b->range = (float)proto_data_num(&msg, "range", 15);
        b->born = GetTime();
        b->own = strcmp(by, app->name) == 0;
        sound_play(SND_SWING);
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
        if (app->conn == CONN_AUTH_SENT || app->conn == CONN_HELLO_SENT) {
            char why[160];

            snprintf(why, sizeof(why), "%s", proto_data_str(&msg, "message", "unknown error"));
            proto_msg_free(&msg);
            disconnect(app, why);
            return;
        }
        snprintf(app->error, sizeof(app->error), "%s",
                 proto_data_str(&msg, "message", "unknown error"));
        if (app->screen == SCREEN_WORLD) {
            snprintf(line, sizeof(line), "error: %s", app->error);
            log_line(app, line);
        }
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
    app->console_open = 1;
    app->console_scroll = 0;
    app->cmd[0] = '\0';
    app->count = 0;
    app->pending_z = 0;
    app->pending_op = 0;
    app->hist_nav = -1;
    app->comp_idx = -1;
}

static void history_push(app_t *app, const char *line)
{
    int i;

    if (line[0] == '\0') return;
    if (app->hist_count > 0 && strcmp(app->cmd_history[app->hist_count - 1], line) == 0) return;

    if (app->hist_count == MAX_HISTORY) {
        for (i = 1; i < MAX_HISTORY; i++) strcpy(app->cmd_history[i - 1], app->cmd_history[i]);
        app->hist_count--;
    }
    snprintf(app->cmd_history[app->hist_count], sizeof(app->cmd_history[0]), "%s", line);
    app->hist_count++;
    history_save(app);
}

static const char *COMMANDS[] = { "connect", "disconnect", "mute", "q", "retry", "setup" };
#define COMMAND_COUNT 6

/* tab cycles through commands matching the typed prefix */
static void cmd_tab_complete(app_t *app)
{
    int matches[COMMAND_COUNT];
    int found = 0;
    int i;
    size_t blen;

    if (strchr(app->cmd, ' ') != NULL) return;    /* only the command word completes */

    if (app->comp_idx == -1) snprintf(app->comp_base, sizeof(app->comp_base), "%s", app->cmd);
    blen = strlen(app->comp_base);

    for (i = 0; i < COMMAND_COUNT; i++) {
        if (strncmp(COMMANDS[i], app->comp_base, blen) == 0) {
            matches[found] = i;
            found++;
        }
    }
    if (found == 0) return;
    app->comp_idx = (app->comp_idx + 1) % found;
    snprintf(app->cmd, sizeof(app->cmd), "%s", COMMANDS[matches[app->comp_idx]]);
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
    } else if (strcmp(cmd, "retry") == 0) {
        if (app->conn == CONN_AUTHED) {
            app->seq++;
            send_raw(app, proto_retry(app->seq));
        }
    } else if (strcmp(cmd, "mute") == 0) {
        sound_toggle_mute();
        if (app->screen == SCREEN_WORLD) {
            if (sound_muted()) log_line(app, "sound off");
            else log_line(app, "sound on");
        }
    } else if (strcmp(cmd, "setup") == 0) {
        if (app->conn != CONN_IDLE) disconnect(app, NULL);
        app->error[0] = '\0';
        app->focus = 0;
        app->screen = SCREEN_SETUP;
    } else if (cmd[0] != '\0') {
        snprintf(line, sizeof(line), "not a command: %s", cmd);
        snprintf(app->error, sizeof(app->error), "%s", line);
        log_line(app, line);
    }
}

static void update_console(app_t *app)
{
    int ch;
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int stored = app->event_total < MAX_EVENTS ? app->event_total : MAX_EVENTS;
    size_t len = strlen(app->cmd);
    char echo[160];

    if (IsKeyPressed(KEY_ESCAPE)) {
        app->console_open = 0;
        return;
    }
    if (IsKeyPressed(KEY_ENTER)) {
        app->console_open = 0;
        if (app->cmd[0] != '\0') {
            snprintf(echo, sizeof(echo), ":%s", app->cmd);
            log_line(app, echo);
        }
        history_push(app, app->cmd);
        run_command(app);
        return;
    }

    /* log scrolling: ctrl-u/ctrl-d or page up/down */
    if (IsKeyPressed(KEY_PAGE_UP) || (ctrl && IsKeyPressed(KEY_U))) {
        app->console_scroll += 5;
        if (app->console_scroll > stored - 1) app->console_scroll = stored - 1;
        if (app->console_scroll < 0) app->console_scroll = 0;
        return;
    }
    if (IsKeyPressed(KEY_PAGE_DOWN) || (ctrl && IsKeyPressed(KEY_D))) {
        app->console_scroll -= 5;
        if (app->console_scroll < 0) app->console_scroll = 0;
        return;
    }

    if (IsKeyPressed(KEY_TAB)) {
        cmd_tab_complete(app);
        return;
    }

    /* arrows walk the command history, vim-style */
    if (IsKeyPressed(KEY_UP) && app->hist_nav < app->hist_count - 1) {
        if (app->hist_nav == -1) snprintf(app->cmd_stash, sizeof(app->cmd_stash), "%s", app->cmd);
        app->hist_nav++;
        app->comp_idx = -1;
        snprintf(app->cmd, sizeof(app->cmd), "%s",
                 app->cmd_history[app->hist_count - 1 - app->hist_nav]);
        return;
    }
    if (IsKeyPressed(KEY_DOWN) && app->hist_nav >= 0) {
        app->hist_nav--;
        if (app->hist_nav == -1) {
            snprintf(app->cmd, sizeof(app->cmd), "%s", app->cmd_stash);
        } else {
            snprintf(app->cmd, sizeof(app->cmd), "%s",
                     app->cmd_history[app->hist_count - 1 - app->hist_nav]);
        }
        return;
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (len == 0) {
            app->console_open = 0;    /* backspace on an empty prompt closes, like vim */
            return;
        }
        app->cmd[len - 1] = '\0';
        len--;
        app->comp_idx = -1;
    }

    for (;;) {
        ch = GetCharPressed();
        if (ch == 0) break;
        if ((ch == '`' || ch == '~') && len == 0) {
            app->console_open = 0;    /* ~ on an empty prompt toggles the console shut */
            return;
        }
        if (ch < 32 || ch > 126) continue;
        if (len + 1 >= sizeof(app->cmd)) continue;
        app->cmd[len++] = (char)ch;
        app->cmd[len] = '\0';
        app->comp_idx = -1;
    }
}

/* ---------------- input: setup + home ---------------- */

static void text_input(char *buf, size_t cap)
{
    int ch;
    size_t len = strlen(buf);

    for (;;) {
        ch = GetCharPressed();
        if (ch == 0) break;
        if (ch < 32 || ch > 126) continue;
        if (len + 1 >= cap) continue;
        buf[len++] = (char)ch;
        buf[len] = '\0';
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (len > 0) buf[len - 1] = '\0';
    }
}

static void update_setup_screen(app_t *app)
{
    if (IsKeyPressed(KEY_TAB)) app->focus = 1 - app->focus;

    if (app->focus == 0) text_input(app->name, sizeof(app->name));
    else text_input(app->password, sizeof(app->password));

    if (IsKeyPressed(KEY_ENTER)) {
        if (app->name[0] == '\0') {
            snprintf(app->error, sizeof(app->error), "name is empty");
            return;
        }
        if (app->password[0] == '\0') {
            snprintf(app->error, sizeof(app->error), "password is empty");
            return;
        }
        app->error[0] = '\0';
        config_save(app);
        app->screen = SCREEN_HOME;
    }
}

static void update_home_screen(app_t *app)
{
    int ch;

    for (;;) {
        ch = GetCharPressed();
        if (ch == 0) break;
        if (ch == ':' || ch == '~' || ch == '`') {
            cmd_start(app);
            return;
        }
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
    if (self_busy(app)) return;

    /* optimistic echo: predict with the same rule the server runs.
     * a dash moves `mult` tiles per requested count; the server derives the
     * same multiplier from the (capital) motion char. */
    tiles = count * mult;
    if (app->move_cost > 0) {
        int affordable = (int)(app->stamina / (float)app->move_cost);
        if (tiles > affordable) tiles = affordable;
    }
    if (tiles <= 0) return;

    if (dx > 0) app->self_facing = 'l';
    if (dx < 0) app->self_facing = 'h';
    if (dy > 0) app->self_facing = 'j';
    if (dy < 0) app->self_facing = 'k';

    granted = local_resolve_move(app, dx, dy, tiles, &x, &y);
    if (granted > 0) {
        sound_play(SND_STEP);
        app->self_moved_at = GetTime();
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

static int self_busy(app_t *app)
{
    return GetTime() < app->self_recover_until;
}

static void do_melee(app_t *app, const char *motion)
{
    int count = app->count;
    int pool;

    if (count == 0) count = 1;
    app->count = 0;
    record_op(app, 2, motion, count);
    if (self_busy(app)) return;

    if (motion[0] == 'h' || motion[0] == 'j' || motion[0] == 'k' || motion[0] == 'l') {
        app->self_facing = motion[0];
    }

    pool = count;
    if (app->melee_cost > 0) {
        int affordable = (int)(app->stamina / (float)app->melee_cost);
        if (pool > affordable) pool = affordable;
    }
    if (pool > 0) {
        float windup = (float)pool * app->melee_windup;
        float recover = (float)pool * app->melee_recover;

        if (windup > app->windup_max) windup = app->windup_max;
        if (recover > app->recover_max) recover = app->recover_max;
        app->self_windup_until = GetTime() + windup;
        app->self_recover_until = app->self_windup_until + recover;
        app->self_windup_len = windup;

        app->stamina -= (float)(pool * app->melee_cost);
        if (app->stamina < 0) app->stamina = 0;
    }

    app->seq++;
    app->outstanding++;
    send_raw(app, proto_intent(app->seq, "melee", motion, count));
}

static void send_defend(app_t *app, int on)
{
    app->seq++;
    app->outstanding++;
    send_raw(app, proto_intent(app->seq, "defend", on ? "on" : "off", 1));
}

static void do_magic(app_t *app, const char *motion)
{
    int count = app->count;
    int pool;
    spell_t *sp;

    if (count == 0) count = 1;
    app->count = 0;
    record_op(app, 3, motion, count);
    if (self_busy(app)) return;
    if (app->spell_count == 0) return;
    sp = &app->spells[app->active_spell];

    if (motion[0] == 'h' || motion[0] == 'j' || motion[0] == 'k' || motion[0] == 'l') {
        app->self_facing = motion[0];
    }

    pool = count;
    if (sp->cost > 0) {
        int affordable = (int)(app->stamina / sp->cost);
        if (pool > affordable) pool = affordable;
    }
    if (pool > 0) {
        float windup = (float)pool * sp->windup;
        float recover = (float)pool * sp->recover;

        if (windup > app->windup_max) windup = app->windup_max;
        if (recover > app->recover_max) recover = app->recover_max;
        app->self_windup_until = GetTime() + windup;
        app->self_recover_until = app->self_windup_until + recover;
        app->self_windup_len = windup;

        app->stamina -= (float)pool * sp->cost;
        if (app->stamina < 0) app->stamina = 0;
    }

    app->seq++;
    app->outstanding++;
    send_raw(app, proto_intent(app->seq, "magic", motion, count));
}

static void update_world_screen(app_t *app)
{
    int ch;

    if (app->book_open) {
        for (;;) {
            ch = GetCharPressed();
            if (ch == 0) break;
            if (ch == 's' || ch == 'q') app->book_open = 0;
            else if (ch == 'j' && app->book_sel < app->spell_count - 1) app->book_sel++;
            else if (ch == 'k' && app->book_sel > 0) app->book_sel--;
        }
        if (IsKeyPressed(KEY_ESCAPE)) app->book_open = 0;
        if (IsKeyPressed(KEY_ENTER) && app->book_sel < app->spell_count) {
            app->book_open = 0;
            app->seq++;
            send_raw(app, proto_spell(app->seq, app->spells[app->book_sel].name));
        }
        return;
    }

    /* ctrl-hjkl: turn in place (steers the guard too) */
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
        char face = 0;

        if (IsKeyPressed(KEY_H)) face = 'h';
        if (IsKeyPressed(KEY_J)) face = 'j';
        if (IsKeyPressed(KEY_K)) face = 'k';
        if (IsKeyPressed(KEY_L)) face = 'l';
        if (face != 0 && !self_busy(app)) {
            char m[2];

            m[0] = face;
            m[1] = '\0';
            app->self_facing = face;
            app->seq++;
            app->outstanding++;
            send_raw(app, proto_intent(app->seq, "face", m, 1));
        }
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        app->count = 0;
        app->pending_z = 0;
        app->pending_op = 0;
        if (app->defending) {
            app->defending = 0;
            app->defend_off_wanted = 1;
            send_defend(app, 0);
            sound_play(SND_GUARD_OFF);
        }
    }

    for (;;) {
        ch = GetCharPressed();
        if (ch == 0) break;

        /* guard up: only x (release), esc, ~ and : work */
        if (app->defending) {
            if (ch == 'x') {
                app->defending = 0;
                app->defend_off_wanted = 1;
                send_defend(app, 0);
                sound_play(SND_GUARD_OFF);
            } else if (ch == '`' || ch == '~' || ch == ':') {
                cmd_start(app);
                return;
            }
            continue;
        }

        if (app->pending_op == 'd' || app->pending_op == 'm') {
            int was_magic = app->pending_op == 'm';

            app->pending_op = 0;
            switch (ch) {
            case 'h':
            case 'j':
            case 'k':
            case 'l': {
                char m[2];

                m[0] = (char)ch;
                m[1] = '\0';
                if (was_magic) do_magic(app, m);
                else do_melee(app, m);
                break;
            }
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
        case '~': cmd_start(app); return;
        case 'x':
            if (self_busy(app)) break;
            app->defending = 1;
            app->defend_off_wanted = 0;
            app->count = 0;
            send_defend(app, 1);
            sound_play(SND_GUARD_ON);
            break;
        case 'z': app->pending_z = 1; break;
        case 'd': app->pending_op = 'd'; break;
        case 'm': app->pending_op = 'm'; break;
        case 's':
            app->book_open = 1;
            app->book_sel = app->active_spell;
            app->count = 0;
            break;
        case '.':                                  /* repeat last melee or cast */
            if (app->last_kind != 2 && app->last_kind != 3) { app->count = 0; break; }
            if (app->count > 0) app->last_count = app->count;   /* <count>. overrides */
            app->count = app->last_count;
            if (app->last_kind == 3) do_magic(app, app->last_motion);
            else do_melee(app, app->last_motion);
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

    px_text(label, x, y, 20, DIM);
    px_text(shown, x + 130, y, 20, color);
    if (focused) px_text("_", x + 130 + px_measure(shown, 20) + 2, y, 20, ACCENT);
}

static void draw_status_bar(app_t *app)
{
    char status[192];
    char count_str[16];
    const char *state = "offline";


    if (app->conn == CONN_SOCKET) state = "connecting";
    if (app->conn == CONN_HELLO_SENT) state = "handshake";
    if (app->conn == CONN_GREETED || app->conn == CONN_AUTH_SENT) state = "connected";
    if (app->conn == CONN_AUTHED) state = "online";

    if (app->screen == SCREEN_WORLD) {
        int bx = GetScreenWidth() - 340;
        int by = GetScreenHeight() - 24;

        const char *mode = app->defending ? "DEFEND" : "NORMAL";

        const char *spell = "-";

        if (app->spell_count > 0) spell = app->spells[app->active_spell].name;
        if (app->challenge_active && strcmp(app->challenge_status, "run") == 0) {
            snprintf(status, sizeof(status), " %s | %d,%d | %s | %.0fms | %s | %.1fs",
                     mode, app->self_x, app->self_y, spell, app->rtt_ms,
                     app->challenge_objective, GetTime() - app->challenge_start);
        } else {
            snprintf(status, sizeof(status), " %s | %d,%d | %s | %.0fms",
                     mode, app->self_x, app->self_y, spell, app->rtt_ms);
        }

        px_text("hp", bx, by, 16, DIM);
        DrawRectangle(bx + 24, by + 4, 84, 8, (Color){ 50, 50, 50, 255 });
        if (app->hp_max > 0) {
            DrawRectangle(bx + 24, by + 4, (int)(84.0f * app->hp_shown / app->hp_max), 8, ACCENT);
        }
        px_text("st", bx + 122, by, 16, DIM);
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
    px_text(status, 10, GetScreenHeight() - 26, 18, DIM);

    if (app->screen == SCREEN_WORLD && (app->count > 0 || app->pending_op != 0)) {
        if (app->count > 0 && app->pending_op != 0) {
            snprintf(count_str, sizeof(count_str), "%d%c", app->count, (char)app->pending_op);
        } else if (app->pending_op != 0) {
            snprintf(count_str, sizeof(count_str), "%c", (char)app->pending_op);
        } else {
            snprintf(count_str, sizeof(count_str), "%d", app->count);
        }
        px_text(count_str, GetScreenWidth() - 80, GetScreenHeight() - 26, 18, ACCENT);
    }
}

static void draw_setup_screen(app_t *app)
{
    px_text("gridmmo", 40, 40, 32, ACCENT);
    px_text("who are you? (first login on a server registers this identity)", 40, 80, 18, DIM);

    draw_field(40, 140, "name", app->name, app->focus == 0, 0);
    draw_field(40, 175, "password", app->password, app->focus == 1, 1);

    px_text("tab to switch, enter to save", 40, 220, 18, DIM);
    if (app->error[0] != '\0') px_text(app->error, 40, 260, 20, ERRCOL);
}

/* vim :intro style — a few dim centered lines on an otherwise empty screen */
static void draw_home_screen(app_t *app)
{
    const char *lines[5];
    int sizes[5];
    int count = 0;
    int w = GetScreenWidth();
    int y = GetScreenHeight() / 2 - 70;
    int i;

    lines[count] = "gridmmo";
    sizes[count] = 30;
    count++;
    lines[count] = "";
    sizes[count] = 18;
    count++;
    lines[count] = ":connect            join the last server";
    sizes[count] = 18;
    count++;
    lines[count] = ":connect <ip:port>  join another";
    sizes[count] = 18;
    count++;
    lines[count] = ":q                  quit";
    sizes[count] = 18;
    count++;

    for (i = 0; i < count; i++) {
        Color c = (i == 0) ? (Color){ 90, 120, 90, 255 } : DIM;

        px_text(lines[i], w / 2 - px_measure(lines[i], sizes[i]) / 2, y, sizes[i], c);
        y += sizes[i] + 12;
    }

    if (app->conn == CONN_SOCKET || app->conn == CONN_HELLO_SENT || app->conn == CONN_AUTH_SENT) {
        const char *note = "connecting...";
        px_text(note, w / 2 - px_measure(note, 18) / 2, y + 10, 18, FG);
    }
    if (app->error[0] != '\0') {
        px_text(app->error, w / 2 - px_measure(app->error, 18) / 2, y + 40, 18, ERRCOL);
    }
}

/* the guard covers only the faced side now */
static void draw_shield(app_t *app, float wx, float wy, char facing, double now_d)
{
    float pulse = 1.0f + 0.05f * sinf((float)now_d * 6.0f);
    float ox = 0.0f;
    float oz = 0.0f;
    float w = 0.95f;
    float d = 0.14f;
    Vector3 pos;

    (void)app;
    if (facing == 'l') { ox = 0.55f; w = 0.14f; d = 0.95f; }
    if (facing == 'h') { ox = -0.55f; w = 0.14f; d = 0.95f; }
    if (facing == 'j') oz = 0.55f;
    if (facing == 'k') oz = -0.55f;

    pos = (Vector3){ wx + ox, 0.55f, wy + oz };
    DrawCube(pos, w * pulse, 1.1f * pulse, d * pulse, Fade((Color){ 130, 160, 220, 255 }, 0.35f));
    DrawCubeWires(pos, w * pulse, 1.1f * pulse, d * pulse,
                  Fade((Color){ 170, 200, 250, 255 }, 0.8f));
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

    px_text(text, x - 1, y, size, shadow);
    px_text(text, x + 1, y, size, shadow);
    px_text(text, x, y - 1, size, shadow);
    px_text(text, x, y + 1, size, shadow);
    px_text(text, x, y, size, color);
}

static void draw_name_tag(app_t *app, const char *name, float wx, float wy, Color color)
{
    Vector3 above = { wx, 1.35f, wy };
    Vector2 screen = GetWorldToScreen(above, app->camera);
    int width = px_measure(name, 18);

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
    if (app->defending) {
        app->stamina -= app->defend_drain * dt;
        if (app->stamina < 0) app->stamina = 0;
    } else {
        app->stamina = fminf(app->stamina_max, app->stamina + app->stamina_regen * dt);
    }

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
                /* soft look: no hard outline, slightly lower body with a
                 * lighter cap so the top edge reads beveled */
                pos.y = 0.45f;
                DrawCube(pos, 1.0f, 0.9f, 1.0f, WALL_COL);
                pos.y = 0.92f;
                DrawCube(pos, 0.94f, 0.06f, 0.94f, (Color){ 88, 88, 98, 255 });
            }
        }
    }

    /* fire bolts: glowing orbs flying down their line */
    for (i = 0; i < MAX_BOLTS; i++) {
        bolt_t *b = &app->bolts[i];
        float traveled;
        float bx, bz;
        int cell;
        int cx, cy;
        int k;

        if (!b->used) continue;
        traveled = (float)(now_d - b->born) * b->speed;
        if (traveled >= b->range) {
            b->used = 0;
            continue;
        }

        bx = (float)b->x + (float)b->dx * (0.6f + traveled);
        bz = (float)b->y + (float)b->dy * (0.6f + traveled);
        cell = (int)(0.6f + traveled + 0.5f);
        cx = b->x + b->dx * cell;
        cy = b->y + b->dy * cell;

        if (tile_at(app, cx, cy) != 0) {
            /* fizzles against the wall */
            trail_add_burst(app, (float)(b->x + b->dx * (cell - 1)),
                            (float)(b->y + b->dy * (cell - 1)));
            b->used = 0;
            continue;
        }
        if (cell > 0 && (occupied(app, cx, cy) || (cx == app->self_x && cy == app->self_y))) {
            /* bursts on a body; the server confirms the damage */
            trail_add_burst(app, (float)cx, (float)cy);
            sound_play(SND_HIT);
            b->used = 0;
            continue;
        }

        for (k = 0; k < 3; k++) {
            float back = (float)k * 0.45f;
            float px = bx - (float)b->dx * back;
            float pz = bz - (float)b->dy * back;
            float a = k == 0 ? 0.95f : (k == 1 ? 0.4f : 0.15f);
            float s = 0.3f - 0.06f * (float)k;
            float flick = 0.9f + 0.2f * sinf((float)now_d * 18.0f + (float)i * 2.0f);

            DrawCube((Vector3){ px, 0.55f, pz }, s * flick, s * flick, s * flick,
                     Fade((Color){ 255, 200, 90, 255 }, a));
            DrawCube((Vector3){ px, 0.55f, pz }, s * 0.5f, s * 0.5f, s * 0.5f,
                     Fade((Color){ 255, 255, 220, 255 }, a));
        }
    }

    /* telegraphs: threatened tiles pulse until the strike lands */
    for (i = 0; i < MAX_TELEGRAPH; i++) {
        telegraph_t *tg = &app->telegraph[i];
        float pulse;
        Color warn;
        int k;

        if (!tg->used) continue;
        if (now_d > tg->until + 0.05) {
            tg->used = 0;
            continue;
        }
        pulse = 0.22f + 0.14f * sinf((float)now_d * 12.0f);
        if (tg->own) warn = (Color){ 230, 200, 90, 255 };
        else warn = (Color){ 220, 60, 40, 255 };

        for (k = 1; k <= tg->count; k++) {
            float px = (float)(tg->x + tg->dx * k);
            float pz = (float)(tg->y + tg->dy * k);

            DrawCube((Vector3){ px, 0.03f, pz }, 0.85f, 0.02f, 0.85f, Fade(warn, pulse));
        }
    }

    /* melee: a black knife flies down the ray, point first */
    for (i = 0; i < MAX_ATTACK_FX; i++) {
        attack_fx_t *fx = &app->attack_fx[i];
        float life;
        float alpha;
        float travel;
        float angle;
        Color blade;
        Color handle;
        int k;

        if (!fx->used) continue;
        life = ((float)now_d - fx->born) / ATTACK_FX_TTL;
        if (life >= 1.0f) {
            fx->used = 0;
            continue;
        }
        if (life < 0.0f) life = 0.0f;

        alpha = 1.0f;
        if (life > 0.7f) alpha = (1.0f - life) / 0.3f;
        travel = 1.0f + life * (float)(fx->count - 1);      /* first tile -> last tile */

        angle = 0.0f;                                       /* knife modeled along +x */
        if (fx->dx < 0) angle = 180.0f;
        if (fx->dy > 0) angle = -90.0f;
        if (fx->dy < 0) angle = 90.0f;

        blade = (Color){ 24, 24, 30, 255 };
        if (fx->own) handle = (Color){ 200, 170, 90, 255 };
        else handle = (Color){ 190, 60, 50, 255 };

        /* the knife plus one faint motion ghost behind it */
        for (k = 0; k < 2; k++) {
            float at = travel - (float)k * 0.55f;
            float px, pz, a;

            if (at < 1.0f) continue;
            px = (float)fx->x + (float)fx->dx * at;
            pz = (float)fx->y + (float)fx->dy * at;
            a = alpha * (k == 0 ? 1.0f : 0.28f);

            rlPushMatrix();
            rlTranslatef(px, 0.5f, pz);
            rlRotatef(angle, 0.0f, 1.0f, 0.0f);
            DrawCube((Vector3){ 0.08f, 0, 0 }, 0.55f, 0.05f, 0.12f, Fade(blade, a));
            DrawCube((Vector3){ 0.42f, 0, 0 }, 0.16f, 0.04f, 0.06f, Fade(blade, a));
            DrawCube((Vector3){ -0.28f, 0, 0 }, 0.18f, 0.07f, 0.08f, Fade(handle, a));
            rlPopMatrix();
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
        player_t *p = &app->players[i];
        float pmove;

        if (!p->used) continue;
        pmove = 1.0f - (float)((now_d - p->moved_at) / 0.35);
        if (pmove < 0.0f) pmove = 0.0f;
        if (pmove > 1.0f) pmove = 1.0f;
        if (p->figure == FIG_WISP) {
            draw_flame(p->rx, p->ry, name_seed(p->name), 0);
        } else {
            figures_draw(p->figure, p->rx, p->ry, p->facing, (float)now_d, pmove, 0,
                         name_seed(p->name));
        }
        if (app->players[i].defending) {
            draw_shield(app, app->players[i].rx, app->players[i].ry,
                        app->players[i].facing, now_d);
        }
        if (now_d - app->players[i].hit_time < HIT_FLASH) {
            float flash = 1.0f - (float)((now_d - app->players[i].hit_time) / HIT_FLASH);
            Vector3 pos = { app->players[i].rx, 0.5f, app->players[i].ry };
            DrawCube(pos, 0.9f, 1.0f, 0.9f, Fade(WHITE, 0.45f * flash));
        }
    }

    {
        float smove = 1.0f - (float)((now_d - app->self_moved_at) / 0.35);

        if (smove < 0.0f) smove = 0.0f;
        if (smove > 1.0f) smove = 1.0f;
        figures_draw(FIG_HUMAN, app->self_rx, app->self_ry, app->self_facing,
                     (float)now_d, smove, 1, name_seed(app->name));
    }
    if (app->defending) {
        draw_shield(app, app->self_rx, app->self_ry, app->self_facing, now_d);
    }

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

    /* own cast bar: gold fills through the windup, grey drains in recovery */
    if (now_d < app->self_recover_until) {
        Vector3 above = { app->self_rx, 1.5f, app->self_ry };
        Vector2 screen = GetWorldToScreen(above, app->camera);
        float frac;
        Color fill;

        if (now_d < app->self_windup_until && app->self_windup_len > 0.001) {
            frac = 1.0f - (float)((app->self_windup_until - now_d) / app->self_windup_len);
            fill = (Color){ 230, 200, 90, 255 };
        } else {
            fill = (Color){ 130, 130, 140, 255 };
            frac = (float)((app->self_recover_until - now_d) /
                           (app->self_recover_until - app->self_windup_until + 0.0001));
        }
        DrawRectangle((int)screen.x - 16, (int)screen.y - 6, 32, 5, (Color){ 40, 40, 46, 220 });
        DrawRectangle((int)screen.x - 16, (int)screen.y - 6, (int)(32.0f * frac), 5, fill);
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
        width = px_measure(text, size);
        col = (Color){ 255, 90, 70, 255 };
        if (life > 0.5f) col.a = (unsigned char)(255 * (1.0f - (life - 0.5f) / 0.5f));
        draw_text_outlined(text, (int)screen.x - width / 2, (int)screen.y, size, col);
    }

    /* challenge result overlay */
    if (app->challenge_active && strcmp(app->challenge_status, "run") != 0) {
        int w = GetScreenWidth();
        int h = GetScreenHeight();
        char big[96];
        char small[64];
        Color col;

        DrawRectangle(0, 0, w, h, Fade(BG, 0.6f));
        if (strcmp(app->challenge_status, "won") == 0) {
            snprintf(big, sizeof(big), "challenge cleared - %.1fs", app->challenge_time);
            col = ACCENT;
        } else {
            snprintf(big, sizeof(big), "you died - %.1fs", app->challenge_time);
            col = ERRCOL;
        }
        snprintf(small, sizeof(small), ":retry to go again");
        draw_text_outlined(big, w / 2 - px_measure(big, 32) / 2, h / 2 - 40, 32, col);
        draw_text_outlined(small, w / 2 - px_measure(small, 16) / 2, h / 2 + 8, 16, DIM);
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

static void draw_spellbook(app_t *app)
{
    int w = 300;
    int h = 60 + app->spell_count * 26;
    int x = GetScreenWidth() / 2 - w / 2;
    int y = GetScreenHeight() / 2 - h / 2;
    int i;

    DrawRectangle(x, y, w, h, (Color){ 16, 16, 22, 240 });
    DrawRectangleLines(x, y, w, h, (Color){ 90, 90, 110, 255 });
    px_text("spellbook", x + 12, y + 10, 16, DIM);

    for (i = 0; i < app->spell_count; i++) {
        spell_t *sp = &app->spells[i];
        char line[96];
        Color col = (i == app->book_sel) ? ACCENT : FG;
        const char *mark = (i == app->active_spell) ? "*" : " ";

        snprintf(line, sizeof(line), "%s %s   %.0fst/pt  spd %.0f  rng %.0f",
                 mark, sp->name, sp->cost, sp->speed, sp->range);
        px_text(line, x + 12, y + 40 + i * 26, 16, col);
    }
    px_text("j/k move, enter select, esc close", x + 12, y + h - 22, 16, DIM);
}

static void draw_console(app_t *app)
{
    int w = GetScreenWidth();
    int h = GetScreenHeight() / 2;
    int stored = app->event_total < MAX_EVENTS ? app->event_total : MAX_EVENTS;
    int rows = (h - 64) / 20;
    int i;

    char prompt[160];

    DrawRectangle(0, 0, w, h, (Color){ 12, 12, 16, 235 });
    DrawRectangle(0, h, w, 2, (Color){ 90, 90, 110, 255 });
    px_text("console — enter runs, esc closes, up/down history, ctrl-u/d scroll",
             12, 10, 16, DIM);

    /* newest just above the prompt; console_scroll pages upward */
    for (i = 0; i < rows && i < stored; i++) {
        int idx = i + app->console_scroll;    /* 0 = newest */
        int slot;
        int y;

        if (idx >= stored) break;
        slot = ((app->event_head - 1 - idx) % MAX_EVENTS + MAX_EVENTS) % MAX_EVENTS;
        y = h - 52 - i * 20;
        px_text(app->events[slot], 12, y, 16, (Color){ 210, 210, 210, 255 });
    }

    /* the prompt line */
    DrawRectangle(0, h - 28, w, 26, (Color){ 20, 20, 26, 255 });
    snprintf(prompt, sizeof(prompt), ":%s", app->cmd);
    px_text(prompt, 12, h - 24, 18, FG);
    px_text("_", 12 + px_measure(prompt, 18) + 2, h - 24, 18, ACCENT);
}

int main(void)
{
    app_t app;

    srand((unsigned)time(NULL));   /* ws mask/key fallback where /dev/urandom is absent */

    memset(&app, 0, sizeof(app));
    app.conn = CONN_IDLE;
    app.hist_nav = -1;
    app.self_facing = 'j';
    snprintf(app.address, sizeof(app.address), DEFAULT_ADDRESS);
    app.comp_idx = -1;
    if (config_load(&app)) app.screen = SCREEN_HOME;
    else app.screen = SCREEN_SETUP;
    history_load(&app);
    if (app.screen == SCREEN_HOME) {
        char seed[160];

        /* even on a fresh install, up-arrow recalls the last server */
        snprintf(seed, sizeof(seed), "connect %s", app.address);
        history_push(&app, seed);
    }

    app.cam_dist = 14.0f;
    app.camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    app.camera.fovy = 55.0f;
    app.camera.projection = CAMERA_PERSPECTIVE;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1100, 700, "gridmmo");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    sound_init();
    {
        /* rasterize at the font's native 16px grid, thresholded (no AA) */
        GlyphInfo *glyphs = LoadFontData(FONT_TTF, FONT_TTF_LEN, 16, NULL, 95, FONT_BITMAP);

        if (glyphs != NULL) {
            Rectangle *recs = NULL;
            Image atlas = GenImageFontAtlas(glyphs, &recs, 95, 16, 2, 0);

            PX_FONT.baseSize = 16;
            PX_FONT.glyphCount = 95;
            PX_FONT.glyphPadding = 2;
            PX_FONT.glyphs = glyphs;
            PX_FONT.recs = recs;
            PX_FONT.texture = LoadTextureFromImage(atlas);
            UnloadImage(atlas);
            if (PX_FONT.texture.id != 0) PX_OK = 1;
        }
    }

    while (!WindowShouldClose() && !app.want_quit) {
        pump_network(&app);

        if (app.console_open) {
            update_console(&app);
        } else {
            switch (app.screen) {
            case SCREEN_SETUP: update_setup_screen(&app); break;
            case SCREEN_HOME: update_home_screen(&app); break;
            case SCREEN_WORLD: update_world_screen(&app); break;
            }
        }

        BeginDrawing();
        ClearBackground(BG);
        switch (app.screen) {
        case SCREEN_SETUP: draw_setup_screen(&app); break;
        case SCREEN_HOME: draw_home_screen(&app); break;
        case SCREEN_WORLD: draw_world_screen(&app); break;
        }
        if (app.screen == SCREEN_WORLD && app.book_open) draw_spellbook(&app);
        if (app.console_open) draw_console(&app);
        draw_status_bar(&app);
        EndDrawing();
    }

    if (app.ws.state != WS_CLOSED) ws_close(&app.ws);
    if (PX_OK) UnloadFont(PX_FONT);
    sound_shutdown();
    CloseWindow();
    return 0;
}
