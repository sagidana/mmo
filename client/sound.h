#ifndef SOUND_H
#define SOUND_H

enum {
    SND_STEP,
    SND_SWING,
    SND_HIT,
    SND_KILL,
    SND_DEATH,
    SND_GUARD_ON,
    SND_GUARD_OFF,
    SND_GUARD_BREAK,
    SND_COUNT,
};

/* all effects are synthesized at startup; no asset files */
void sound_init(void);
void sound_shutdown(void);
void sound_play(int id);
void sound_toggle_mute(void);
int sound_muted(void);

#endif
