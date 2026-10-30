/* mmd: an editor instance's connection to midimixd's control socket (../midimixd/ctl.h), on a thread of its own.
 * It sends the instance's mapping when it changes and after every reconnect (midimixd keeps it while the connection
 * is open, so it lasts as long as the instance), and asks for midimixd's state every 100 ms. Nothing here blocks the
 * caller: mmd_state() copies the last state read. */
#pragma once

#include "map.h"

#define MMD_STRIPS 8

typedef struct {
    int up;                    /* midimixd answered the last request */
    int midimix, api;          /* the Midimix is there; MPC's state is current */
    int bank, banks, tracks;   /* the bank shown (from 1), how many, the tracks with a strip */
    char mapfrom[8];           /* whose mapping is in effect: "you", "other", "default" */
    char error[96];            /* midimixd refused the last mapping sent: why ("" if it took it) */
    unsigned touch_seq;        /* counts the controls touched on the Midimix (0: none yet) */
    int touch_row;             /* map.h's row of the last one */
    int selected;              /* the selected track's strip (1-8), 0 if it isn't in this bank */
    struct {
        int present;           /* the strip has a track */
        long colour;           /* RGB24, -1 unknown */
        char type[16], name[64];
    } strip[MMD_STRIPS];
} mmd_state_t;

typedef struct mmd mmd_t;

/* path: the control socket. NULL if the thread can't start. */
mmd_t *mmd_start(const char *path, const unsigned char map[MAP_ROWS]);
void mmd_stop(mmd_t *m);
/* The mapping to keep in midimixd: sent at once. */
void mmd_set_map(mmd_t *m, const unsigned char map[MAP_ROWS]);
/* The last state read; returns its revision, which changes whenever the state does. */
unsigned mmd_state(mmd_t *m, mmd_state_t *out);
