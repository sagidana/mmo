#include "figures.h"

#include "raylib.h"

#include <math.h>
#include <string.h>

/* every part is designed facing +z ('j'); fig_box rotates it to the
 * actual facing. ox/oz offset from tile center, oy = part bottom. */
static void fig_box(float wx, float wz, char facing,
                    float ox, float oy, float oz,
                    float w, float h, float d, Color col)
{
    float rx = ox;
    float rz = oz;
    float rw = w;
    float rd = d;

    switch (facing) {
    case 'k': rx = -ox; rz = -oz; break;
    case 'l': rx = -oz; rz = ox; rw = d; rd = w; break;
    case 'h': rx = oz; rz = -ox; rw = d; rd = w; break;
    default: break;
    }
    DrawCube((Vector3){ wx + rx, oy + h / 2.0f, wz + rz }, rw, h, rd, col);
}

static const Color SKIN = { 226, 184, 144, 255 };
static const Color PANTS = { 58, 58, 72, 255 };
static const Color BOOTS = { 40, 40, 48, 255 };
static const Color TINT_SELF = { 110, 170, 110, 255 };
static const Color TINT_OTHER = { 168, 148, 112, 255 };
static const Color BONE = { 214, 209, 198, 255 };
static const Color BONE_DARK = { 90, 88, 84, 255 };

static void draw_humanoid(float wx, float wz, char facing, float t, float move,
                          Color tint, int hooded, Color skin, Color pants)
{
    float swing = move * sinf(t * 10.0f) * 0.09f;
    float bob = (1.0f - move) * 0.012f * sinf(t * 2.6f);

    fig_box(wx, wz, facing, -0.08f, 0.00f, +swing, 0.13f, 0.26f, 0.15f, pants);
    fig_box(wx, wz, facing, +0.08f, 0.00f, -swing, 0.13f, 0.26f, 0.15f, pants);
    fig_box(wx, wz, facing, -0.08f, 0.00f, +swing, 0.14f, 0.07f, 0.16f, BOOTS);
    fig_box(wx, wz, facing, +0.08f, 0.00f, -swing, 0.14f, 0.07f, 0.16f, BOOTS);
    fig_box(wx, wz, facing, 0.0f, 0.26f + bob, 0.0f, 0.34f, 0.28f, 0.18f, tint);
    fig_box(wx, wz, facing, -0.225f, 0.27f + bob, -swing, 0.10f, 0.26f, 0.13f, tint);
    fig_box(wx, wz, facing, +0.225f, 0.27f + bob, +swing, 0.10f, 0.26f, 0.13f, tint);
    fig_box(wx, wz, facing, -0.225f, 0.27f + bob, -swing, 0.09f, 0.06f, 0.12f, skin);
    fig_box(wx, wz, facing, +0.225f, 0.27f + bob, +swing, 0.09f, 0.06f, 0.12f, skin);
    fig_box(wx, wz, facing, 0.0f, 0.54f + bob, +0.02f, 0.30f, 0.26f, 0.28f, skin);
    if (hooded) {
        fig_box(wx, wz, facing, 0.0f, 0.76f + bob, 0.0f, 0.36f, 0.12f, 0.34f, tint);
        fig_box(wx, wz, facing, 0.0f, 0.54f + bob, -0.15f, 0.36f, 0.28f, 0.07f, tint);
    } else {
        /* bare skull: eye sockets */
        fig_box(wx, wz, facing, -0.07f, 0.66f + bob, +0.15f, 0.06f, 0.06f, 0.03f, BONE_DARK);
        fig_box(wx, wz, facing, +0.07f, 0.66f + bob, +0.15f, 0.06f, 0.06f, 0.03f, BONE_DARK);
    }
}

static void draw_slime(float wx, float wz, float t, unsigned seed)
{
    float squash = 0.12f * sinf(t * 4.0f + (float)(seed % 7));
    float h = 0.42f * (1.0f + squash);
    float w = 0.55f * (1.0f - squash * 0.5f);
    Color body = { 110, 190, 150, 235 };
    Color core = { 60, 120, 90, 255 };

    DrawCube((Vector3){ wx, h / 2.0f, wz }, w, h, w, body);
    DrawCube((Vector3){ wx, 0.14f, wz }, 0.22f, 0.2f, 0.22f, core);
}

static void draw_rat(float wx, float wz, char facing, float t, float move)
{
    Color fur = { 120, 105, 95, 255 };
    Color fur_dark = { 90, 78, 70, 255 };
    float wig = move * sinf(t * 14.0f) * 0.05f;

    fig_box(wx, wz, facing, wig, 0.03f, -0.05f, 0.30f, 0.20f, 0.42f, fur);
    fig_box(wx, wz, facing, 0.0f, 0.08f, +0.27f, 0.20f, 0.17f, 0.20f, fur);
    fig_box(wx, wz, facing, -0.07f, 0.25f, +0.28f, 0.06f, 0.07f, 0.03f, fur_dark);
    fig_box(wx, wz, facing, +0.07f, 0.25f, +0.28f, 0.06f, 0.07f, 0.03f, fur_dark);
    fig_box(wx, wz, facing, 0.0f, 0.10f, +0.39f, 0.07f, 0.06f, 0.06f, (Color){ 220, 160, 160, 255 });
    fig_box(wx, wz, facing, -wig, 0.08f, -0.36f, 0.06f, 0.06f, 0.20f, fur_dark);
    fig_box(wx, wz, facing, wig, 0.06f, -0.52f, 0.05f, 0.05f, 0.14f, fur_dark);
}

static void draw_spider(float wx, float wz, char facing, float t, float move)
{
    Color body = { 48, 42, 54, 255 };
    Color joint = { 70, 62, 78, 255 };
    int i;

    fig_box(wx, wz, facing, 0.0f, 0.10f, -0.04f, 0.32f, 0.16f, 0.36f, body);
    fig_box(wx, wz, facing, 0.0f, 0.10f, +0.23f, 0.18f, 0.13f, 0.14f, body);
    fig_box(wx, wz, facing, -0.05f, 0.16f, +0.30f, 0.03f, 0.03f, 0.02f, (Color){ 220, 60, 60, 255 });
    fig_box(wx, wz, facing, +0.05f, 0.16f, +0.30f, 0.03f, 0.03f, 0.02f, (Color){ 220, 60, 60, 255 });

    for (i = 0; i < 4; i++) {
        float z = -0.16f + 0.11f * (float)i;
        float lift_a = 0.02f + move * 0.05f * (0.5f + 0.5f * sinf(t * 12.0f + (float)i * 1.6f));
        float lift_b = 0.02f + move * 0.05f * (0.5f + 0.5f * sinf(t * 12.0f + (float)i * 1.6f + PI));

        fig_box(wx, wz, facing, -0.26f, lift_a, z, 0.16f, 0.05f, 0.06f, joint);
        fig_box(wx, wz, facing, +0.26f, lift_b, z, 0.16f, 0.05f, 0.06f, joint);
    }
}

static void draw_golem(float wx, float wz, char facing, float t)
{
    Color stone = { 88, 88, 98, 255 };
    Color stone_dark = { 66, 66, 76, 255 };
    float sway = 0.015f * sinf(t * 1.6f);

    fig_box(wx, wz, facing, -0.14f, 0.00f, 0.0f, 0.20f, 0.34f, 0.24f, stone_dark);
    fig_box(wx, wz, facing, +0.14f, 0.00f, 0.0f, 0.20f, 0.34f, 0.24f, stone_dark);
    fig_box(wx, wz, facing, 0.0f, 0.34f + sway, 0.0f, 0.52f, 0.48f, 0.32f, stone);
    fig_box(wx, wz, facing, -0.36f, 0.28f + sway, 0.0f, 0.18f, 0.5f, 0.22f, stone_dark);
    fig_box(wx, wz, facing, +0.36f, 0.28f + sway, 0.0f, 0.18f, 0.5f, 0.22f, stone_dark);
    fig_box(wx, wz, facing, 0.0f, 0.82f + sway, +0.02f, 0.28f, 0.22f, 0.26f, stone_dark);
    fig_box(wx, wz, facing, -0.06f, 0.90f + sway, +0.14f, 0.05f, 0.04f, 0.03f, (Color){ 240, 140, 60, 255 });
    fig_box(wx, wz, facing, +0.06f, 0.90f + sway, +0.14f, 0.05f, 0.04f, 0.03f, (Color){ 240, 140, 60, 255 });
}

static void draw_hound(float wx, float wz, char facing, float t, float move)
{
    Color fur = { 104, 88, 76, 255 };
    Color fur_dark = { 78, 64, 56, 255 };
    float gallop = move * sinf(t * 12.0f) * 0.05f;

    fig_box(wx, wz, facing, 0.0f, 0.16f + gallop * 0.5f, -0.02f, 0.26f, 0.24f, 0.52f, fur);
    fig_box(wx, wz, facing, 0.0f, 0.28f, +0.30f, 0.20f, 0.18f, 0.20f, fur);
    fig_box(wx, wz, facing, 0.0f, 0.28f, +0.42f, 0.12f, 0.11f, 0.10f, fur_dark);
    fig_box(wx, wz, facing, -0.07f, 0.46f, +0.28f, 0.05f, 0.08f, 0.03f, fur_dark);
    fig_box(wx, wz, facing, +0.07f, 0.46f, +0.28f, 0.05f, 0.08f, 0.03f, fur_dark);
    fig_box(wx, wz, facing, -0.09f, gallop > 0 ? gallop : 0.0f, +0.18f, 0.08f, 0.18f, 0.08f, fur_dark);
    fig_box(wx, wz, facing, +0.09f, gallop < 0 ? -gallop : 0.0f, +0.18f, 0.08f, 0.18f, 0.08f, fur_dark);
    fig_box(wx, wz, facing, -0.09f, gallop < 0 ? -gallop : 0.0f, -0.20f, 0.08f, 0.18f, 0.08f, fur_dark);
    fig_box(wx, wz, facing, +0.09f, gallop > 0 ? gallop : 0.0f, -0.20f, 0.08f, 0.18f, 0.08f, fur_dark);
    fig_box(wx, wz, facing, 0.0f, 0.34f, -0.32f, 0.06f, 0.06f, 0.16f, fur_dark);
}

static void draw_crab(float wx, float wz, char facing, float t, float move)
{
    Color shell = { 190, 84, 64, 255 };
    Color shell_light = { 214, 108, 84, 255 };
    Color leg = { 140, 60, 48, 255 };
    float snip = 0.03f * sinf(t * 6.0f);
    int i;

    fig_box(wx, wz, facing, 0.0f, 0.05f, 0.0f, 0.50f, 0.20f, 0.36f, shell);
    fig_box(wx, wz, facing, 0.0f, 0.25f, 0.0f, 0.40f, 0.06f, 0.28f, shell_light);
    fig_box(wx, wz, facing, -0.30f, 0.08f + snip, +0.20f, 0.16f, 0.14f, 0.16f, shell_light);
    fig_box(wx, wz, facing, +0.30f, 0.08f - snip, +0.20f, 0.16f, 0.14f, 0.16f, shell_light);
    fig_box(wx, wz, facing, -0.06f, 0.28f, +0.16f, 0.04f, 0.08f, 0.03f, (Color){ 235, 235, 235, 255 });
    fig_box(wx, wz, facing, +0.06f, 0.28f, +0.16f, 0.04f, 0.08f, 0.03f, (Color){ 235, 235, 235, 255 });
    for (i = 0; i < 3; i++) {
        float z = -0.12f + 0.11f * (float)i;
        float lift = move * 0.04f * (0.5f + 0.5f * sinf(t * 10.0f + (float)i * 2.1f));

        fig_box(wx, wz, facing, -0.31f, lift, z, 0.12f, 0.05f, 0.05f, leg);
        fig_box(wx, wz, facing, +0.31f, 0.04f - lift, z, 0.12f, 0.05f, 0.05f, leg);
    }
}

static void draw_watcher(float wx, float wz, char facing, float t, unsigned seed)
{
    float hover = 0.42f + 0.06f * sinf(t * 2.0f + (float)(seed % 9));
    Color ball = { 222, 226, 232, 255 };
    Color lid = { 148, 62, 74, 255 };
    Color pupil = { 28, 28, 40, 255 };

    fig_box(wx, wz, facing, 0.0f, hover, 0.0f, 0.40f, 0.40f, 0.40f, ball);
    fig_box(wx, wz, facing, 0.0f, hover + 0.38f, 0.0f, 0.42f, 0.07f, 0.42f, lid);
    fig_box(wx, wz, facing, 0.0f, hover + 0.10f, +0.19f, 0.16f, 0.16f, 0.04f, pupil);
    /* a faint shadow so it reads grounded */
    DrawCube((Vector3){ wx, 0.02f, wz }, 0.3f, 0.02f, 0.3f, (Color){ 0, 0, 0, 90 });
}

static void draw_worm(float wx, float wz, char facing, float t)
{
    Color flesh = { 142, 92, 158, 255 };
    Color flesh_dark = { 108, 66, 122, 255 };
    int i;

    for (i = 0; i < 5; i++) {
        float z = 0.22f - 0.15f * (float)i;
        float rise = 0.06f * fabsf(sinf(t * 3.0f - (float)i * 0.9f));
        float size = 0.26f - 0.03f * (float)i;
        Color col = (i % 2 == 0) ? flesh : flesh_dark;

        fig_box(wx, wz, facing, 0.0f, rise, z, size, size, 0.14f, col);
    }
    fig_box(wx, wz, facing, -0.06f, 0.16f, +0.29f, 0.04f, 0.04f, 0.02f, (Color){ 240, 220, 120, 255 });
    fig_box(wx, wz, facing, +0.06f, 0.16f, +0.29f, 0.04f, 0.04f, 0.02f, (Color){ 240, 220, 120, 255 });
}

int figures_from_name(const char *name)
{
    if (strcmp(name, "slime") == 0) return FIG_SLIME;
    if (strcmp(name, "rat") == 0) return FIG_RAT;
    if (strcmp(name, "spider") == 0) return FIG_SPIDER;
    if (strcmp(name, "golem") == 0) return FIG_GOLEM;
    if (strcmp(name, "skeleton") == 0) return FIG_SKELETON;
    if (strcmp(name, "hound") == 0) return FIG_HOUND;
    if (strcmp(name, "crab") == 0) return FIG_CRAB;
    if (strcmp(name, "watcher") == 0) return FIG_WATCHER;
    if (strcmp(name, "worm") == 0) return FIG_WORM;
    if (strcmp(name, "wisp") == 0) return FIG_WISP;
    return FIG_HUMAN;
}

void figures_draw(int figure, float wx, float wz, char facing, float t,
                  float move, int self, unsigned seed)
{
    float ts = t + (float)(seed % 31) * 0.37f;    /* desync animations */

    switch (figure) {
    case FIG_SLIME: draw_slime(wx, wz, ts, seed); break;
    case FIG_RAT: draw_rat(wx, wz, facing, ts, move); break;
    case FIG_SPIDER: draw_spider(wx, wz, facing, ts, move); break;
    case FIG_GOLEM: draw_golem(wx, wz, facing, ts); break;
    case FIG_SKELETON:
        draw_humanoid(wx, wz, facing, ts, move, BONE, 0, BONE, BONE_DARK);
        break;
    case FIG_HOUND: draw_hound(wx, wz, facing, ts, move); break;
    case FIG_CRAB: draw_crab(wx, wz, facing, ts, move); break;
    case FIG_WATCHER: draw_watcher(wx, wz, facing, ts, seed); break;
    case FIG_WORM: draw_worm(wx, wz, facing, ts); break;
    default:
        draw_humanoid(wx, wz, facing, ts, move, self ? TINT_SELF : TINT_OTHER, 1, SKIN, PANTS);
        break;
    }
}
