#include "sound.h"

#include "raylib.h"

#include <math.h>
#include <stdlib.h>

#define RATE 22050

static Sound sounds[SND_COUNT];
static int ready = 0;
static int muted = 0;

static float frand(void)
{
    return (float)rand() / (float)RAND_MAX;
}

static float noise(void)
{
    return frand() * 2.0f - 1.0f;
}

/* exponential decay envelope: 1 -> ~0 over the buffer, sharper with k */
static float env(int i, int n, float k)
{
    return expf(-k * (float)i / (float)n);
}

static Sound finish(float *buf, int frames)
{
    short *pcm = malloc((size_t)frames * sizeof(short));
    Wave wave;
    Sound sound;
    int i;

    for (i = 0; i < frames; i++) {
        float v = buf[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = (short)(v * 32000.0f);
    }
    wave.frameCount = (unsigned int)frames;
    wave.sampleRate = RATE;
    wave.sampleSize = 16;
    wave.channels = 1;
    wave.data = pcm;
    sound = LoadSoundFromWave(wave);
    free(pcm);
    free(buf);
    return sound;
}

/* short soft click: a fast-decaying low sine with a whisper of noise */
static Sound make_step(void)
{
    int n = RATE * 30 / 1000;
    float *buf = malloc((size_t)n * sizeof(float));
    int i;

    for (i = 0; i < n; i++) {
        float t = (float)i / RATE;
        buf[i] = (sinf(2.0f * PI * 190.0f * t) * 0.8f + noise() * 0.15f) * env(i, n, 9.0f) * 0.35f;
    }
    return finish(buf, n);
}

/* rising whoosh: noise + an upward sine sweep */
static Sound make_swing(void)
{
    int n = RATE * 120 / 1000;
    float *buf = malloc((size_t)n * sizeof(float));
    int i;

    for (i = 0; i < n; i++) {
        float p = (float)i / (float)n;
        float t = (float)i / RATE;
        float freq = 300.0f + 700.0f * p;
        float body = sinf(2.0f * PI * freq * t) * 0.35f + noise() * 0.5f;
        float shape = sinf(PI * p);    /* fade in and out */
        buf[i] = body * shape * 0.4f;
    }
    return finish(buf, n);
}

/* impact: noise crack over a low thump */
static Sound make_hit(void)
{
    int n = RATE * 90 / 1000;
    float *buf = malloc((size_t)n * sizeof(float));
    int i;

    for (i = 0; i < n; i++) {
        float t = (float)i / RATE;
        float thump = sinf(2.0f * PI * 120.0f * t) * env(i, n, 6.0f);
        float crack = noise() * env(i, n, 14.0f);
        buf[i] = (thump * 0.7f + crack * 0.5f) * 0.6f;
    }
    return finish(buf, n);
}

/* kill: descending sweep with a noise tail */
static Sound make_kill(void)
{
    int n = RATE * 300 / 1000;
    float *buf = malloc((size_t)n * sizeof(float));
    float phase = 0.0f;
    int i;

    for (i = 0; i < n; i++) {
        float p = (float)i / (float)n;
        float freq = 420.0f - 340.0f * p;
        phase += 2.0f * PI * freq / RATE;
        buf[i] = (sinf(phase) * 0.6f + noise() * 0.25f * (1.0f - p)) * env(i, n, 3.5f) * 0.55f;
    }
    return finish(buf, n);
}

/* own death: longer, lower descent with tremolo */
static Sound make_death(void)
{
    int n = RATE * 600 / 1000;
    float *buf = malloc((size_t)n * sizeof(float));
    float phase = 0.0f;
    int i;

    for (i = 0; i < n; i++) {
        float p = (float)i / (float)n;
        float t = (float)i / RATE;
        float freq = 300.0f - 240.0f * p;
        float trem = 0.75f + 0.25f * sinf(2.0f * PI * 9.0f * t);
        phase += 2.0f * PI * freq / RATE;
        buf[i] = sinf(phase) * trem * env(i, n, 2.5f) * 0.6f;
    }
    return finish(buf, n);
}

static Sound make_sweep(float from, float to, int ms, float vol)
{
    int n = RATE * ms / 1000;
    float *buf = malloc((size_t)n * sizeof(float));
    float phase = 0.0f;
    int i;

    for (i = 0; i < n; i++) {
        float p = (float)i / (float)n;
        float freq = from + (to - from) * p;
        phase += 2.0f * PI * freq / RATE;
        buf[i] = sinf(phase) * sinf(PI * p) * vol;
    }
    return finish(buf, n);
}

/* guard break: sharp crack plus a low groan */
static Sound make_break(void)
{
    int n = RATE * 200 / 1000;
    float *buf = malloc((size_t)n * sizeof(float));
    int i;

    for (i = 0; i < n; i++) {
        float t = (float)i / RATE;
        float crack = noise() * env(i, n, 10.0f);
        float low = sinf(2.0f * PI * 90.0f * t) * env(i, n, 4.0f);
        buf[i] = (crack * 0.6f + low * 0.6f) * 0.6f;
    }
    return finish(buf, n);
}

void sound_init(void)
{
    InitAudioDevice();
    if (!IsAudioDeviceReady()) return;

    sounds[SND_STEP] = make_step();
    sounds[SND_SWING] = make_swing();
    sounds[SND_HIT] = make_hit();
    sounds[SND_KILL] = make_kill();
    sounds[SND_DEATH] = make_death();
    sounds[SND_GUARD_ON] = make_sweep(220.0f, 520.0f, 90, 0.35f);
    sounds[SND_GUARD_OFF] = make_sweep(430.0f, 240.0f, 70, 0.3f);
    sounds[SND_GUARD_BREAK] = make_break();
    SetMasterVolume(0.6f);
    ready = 1;
}

void sound_shutdown(void)
{
    int i;

    if (ready) {
        for (i = 0; i < SND_COUNT; i++) UnloadSound(sounds[i]);
    }
    CloseAudioDevice();
}

void sound_play(int id)
{
    if (!ready || muted) return;
    if (id < 0 || id >= SND_COUNT) return;

    /* slight random pitch so repeats do not sound mechanical */
    SetSoundPitch(sounds[id], 0.94f + frand() * 0.12f);
    PlaySound(sounds[id]);
}

void sound_toggle_mute(void)
{
    muted = !muted;
}

int sound_muted(void)
{
    return muted;
}