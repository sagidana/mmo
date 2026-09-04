#ifndef FIGURES_H
#define FIGURES_H

enum {
    FIG_HUMAN,
    FIG_SLIME,
    FIG_RAT,
    FIG_SPIDER,
    FIG_GOLEM,
    FIG_SKELETON,
    FIG_HOUND,
    FIG_CRAB,
    FIG_WATCHER,
    FIG_WORM,
    FIG_WISP,      /* rendered by the caller's flame; figures_draw ignores it */
};

/* name from the wire -> figure id (unknown -> FIG_HUMAN) */
int figures_from_name(const char *name);

/* draws a figure standing on tile-center (wx, wz).
 * facing: h/j/k/l (j = toward the camera). t: clock. move: 0 idle .. 1 walking.
 * self: identity tint for humanoids (1 = own color). seed: desyncs animation. */
void figures_draw(int figure, float wx, float wz, char facing, float t,
                  float move, int self, unsigned seed);

#endif
