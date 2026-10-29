/* The Midimix editor as an MPC plugin engine (mpc-vst-plugins wrapper/engine.h): a silent instrument whose page sets
 * what each row of the Midimix's strips does (../midimixd/map.h), the same for the 8 strips.
 *
 * - The mapping is the instance's: saved with the project (the "state" chunk is its text, as map_format writes it)
 *   and sent to midimixd (mmd.c), which uses it while the instance's connection is open and goes back to its own
 *   default once the instance is gone (another project, the track deleted). midimixd isn't needed to edit it.
 * - The page shows the strips of the bank midimixd shows: their tracks' names and colours (colour_<n>: RGB24 rounded
 *   to a palette of 12 hues and grey, images/colour_*.svg, NONE for a strip without a track); the 7 rows (row_<n>, a
 *   list: tap one to pick it, or touch one of its controls on the Midimix); and the picked row's targets (edit_level
 *   or edit_button, the one rowkind says).
 * - The editor's own track is a strip like any other: the API can't tell it apart yet (a VST2's plugin isn't named).
 * - MPC sets parameter 0 to the far end of its range and back on insert and on every STOP (vst2_wrap.c): parameter 0
 *   is the status line, which takes nothing.
 * - MIDIMIXD_SOCKET in the environment overrides the control socket's path (tests). */
#define _GNU_SOURCE
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ctl.h"
#include "engine.h"
#include "map.h"
#include "mmd.h"

static const char *const ROW_LABEL[MAP_ROWS] = {"TOP KNOB", "MIDDLE KNOB", "BOTTOM KNOB", "FADER", "MUTE", "SOLO",
                                                "REC ARM"};
static const char *const ROW_TITLE[MAP_ROWS] = {"TOP KNOBS", "MIDDLE KNOBS", "BOTTOM KNOBS", "FADERS", "MUTE BUTTONS",
                                                "SOLO: MUTE BUTTONS WITH SOLO HELD", "REC ARM BUTTONS"};
static const char *const TARGET_LABEL[MAP_TARGETS] = {
    "NONE", "VOLUME", "PAN", "SEND 1", "SEND 2", "SEND 3", "SEND 4",
    "MUTE", "SOLO", "REC ARM", "INSERT 1", "INSERT 2", "INSERT 3", "INSERT 4", "SELECT",
};

enum { COLOUR_NONE, COLOUR_GREY, COLOUR_HUES };   /* colour_<n>'s options: then 12 hues from red, 30 degrees apart */

typedef struct {
    mmd_t *mmd;
    unsigned char map[MAP_ROWS];
    int row;               /* the row picked */
    unsigned seen_touch;   /* the Midimix touch last acted on; UINT_MAX: none read yet */
    unsigned rev, mrev;    /* this instance's revision (the row picked), mmd's */
    mmd_state_t s;         /* mmd's, as last looked at */
} inst_t;

static void *create(const char *data_dir)
{
    (void)data_dir;
    inst_t *in = calloc(1, sizeof *in);
    const char *path = getenv("MIDIMIXD_SOCKET");
    if (!in)
        return NULL;
    memcpy(in->map, map_default, sizeof in->map);
    in->row = ROW_FADER;
    in->seen_touch = UINT_MAX;
    if (!(in->mmd = mmd_start(path ? path : CTL_SOCKET, in->map))) {
        free(in);
        return NULL;
    }
    return in;
}

static void destroy(void *p)
{
    inst_t *in = p;
    mmd_stop(in->mmd);   /* closes the connection: midimixd drops this mapping */
    free(in);
}

static void midi(void *p, const uint8_t *msg, int len)
{
    (void)p, (void)msg, (void)len;
}

static void render(void *p, int16_t *out, int frames)
{
    (void)p;
    memset(out, 0, frames * 2 * sizeof *out);
}

/* Take mmd's state, and follow a control touched on the Midimix to its row. */
static void look(inst_t *in)
{
    in->mrev = mmd_state(in->mmd, &in->s);
    if (!in->s.up) {
        in->seen_touch = UINT_MAX;   /* midimixd's count starts over when it does */
        return;
    }
    if (in->seen_touch != UINT_MAX && in->s.touch_seq && in->s.touch_seq != in->seen_touch && in->row != in->s.touch_row) {
        in->row = in->s.touch_row;
        in->rev++;
    }
    in->seen_touch = in->s.touch_seq;
}

static void set_map(inst_t *in, const unsigned char map[MAP_ROWS])
{
    if (!memcmp(in->map, map, sizeof in->map))
        return;
    memcpy(in->map, map, sizeof in->map);
    in->rev++;
    mmd_set_map(in->mmd, in->map);
}

/* A track colour (RGB24) as colour_<n>'s option: grey when it has little colour, else the nearest of 12 hues. */
static int palette(long rgb)
{
    double r = (rgb >> 16 & 255) / 255.0, g = (rgb >> 8 & 255) / 255.0, b = (rgb & 255) / 255.0;
    double hi = fmax(r, fmax(g, b)), lo = fmin(r, fmin(g, b)), d = hi - lo, h;
    if (rgb < 0)
        return COLOUR_GREY;
    if (hi < 0.15 || d / hi < 0.25)
        return COLOUR_GREY;
    if (hi == r)
        h = fmod((g - b) / d, 6);
    else if (hi == g)
        h = (b - r) / d + 2;
    else
        h = (r - g) / d + 4;
    h = h * 60 < 0 ? h * 60 + 360 : h * 60;
    return COLOUR_HUES + (int)lround(h / 30) % 12;
}

static int index_of(const unsigned char *list, int n, int target)
{
    for (int i = 0; i < n; i++)
        if (list[i] == target)
            return i;
    return 0;
}

static int status(const inst_t *in, char *buf, int len)
{
    const mmd_state_t *s = &in->s;
    if (!s->up)
        return snprintf(buf, len, "MIDIMIXD IS NOT RUNNING");
    if (!s->api)
        return snprintf(buf, len, "WAITING FOR MPC");
    if (!s->tracks)
        return snprintf(buf, len, "NO TRACKS IN THE PROJECT");
    int first = (s->bank - 1) * MMD_STRIPS + 1, last = first + MMD_STRIPS - 1;
    if (last > s->tracks)
        last = s->tracks;
    if (first == last)
        return snprintf(buf, len, "BANK %d OF %d: TRACK %d OF %d", s->bank, s->banks, first, s->tracks);
    return snprintf(buf, len, "BANK %d OF %d: TRACKS %d-%d OF %d", s->bank, s->banks, first, last, s->tracks);
}

static int hint(const inst_t *in, char *buf, int len)
{
    const mmd_state_t *s = &in->s;
    if (!s->up)
        return snprintf(buf, len, "THE MAPPING IS STILL SAVED WITH THE PROJECT");
    if (s->error[0])
        return snprintf(buf, len, "MIDIMIXD REFUSED THE MAPPING: %s", s->error);
    if (!strcmp(s->mapfrom, "other"))
        return snprintf(buf, len, "ANOTHER MIDIMIX EDITOR IS IN CHARGE");
    if (!s->midimix)
        return snprintf(buf, len, "NO MIDIMIX CONNECTED");
    return snprintf(buf, len, "TOUCH A CONTROL ON THE MIDIMIX TO PICK ITS ROW");
}

static int help(const inst_t *in, char *buf, int len)
{
    int t = in->map[in->row];
    const char *text = "DOES NOTHING";
    if (t >= T_SEND1 && t <= T_SEND4)
        text = "SENDS ONLY MOVE WITH THE TRACK'S AUTOMATION OFF";
    else if (t >= T_INSERT1 && t <= T_INSERT4)
        text = "INSERTS ONLY SWITCH WITH THE TRACK'S AUTOMATION OFF";
    else if (t == T_VOLUME || t == T_PAN)
        text = "TAKES OVER ONCE IT REACHES THE TRACK'S LEVEL";
    else if (t == T_MUTE)
        text = "BLINKS WHEN ANOTHER TRACK'S SOLO SILENCES IT";
    else if (t == T_SELECT)
        text = "LIT ON THE SELECTED TRACK";
    else if (t != T_NONE)
        text = "LIT WHEN ON";
    return snprintf(buf, len, "%s", text);
}

/* "<key>_<n>" with n in 1..max: n, else 0. */
static int numbered(const char *key, const char *prefix, int max)
{
    size_t n = strlen(prefix);
    if (strncmp(key, prefix, n) || key[n] != '_')
        return 0;
    char *end;
    long k = strtol(key + n + 1, &end, 10);
    return *end || k < 1 || k > max ? 0 : (int)k;
}

static int get_param(void *p, const char *key, char *buf, int len)
{
    inst_t *in = p;
    const mmd_state_t *s = &in->s;
    size_t n = strlen(key);
    int k, on = n > 3 && !strcmp(key + n - 3, "_on");
    char base[32];
    if (!strcmp(key, "display_rev")) {   /* the wrapper's poll, every 100 ms: what the rest is read from until the next */
        look(in);
        return snprintf(buf, len, "%u.%u", in->rev, in->mrev);
    }
    if (!strcmp(key, "state"))
        return map_format(in->map, buf, len);
    if (n >= sizeof base)
        return 0;
    snprintf(base, sizeof base, "%.*s", (int)(on ? n - 3 : n), key);
    if ((k = numbered(base, "row", MAP_ROWS))) {
        if (on)
            return snprintf(buf, len, "%d", in->row == k - 1);
        return snprintf(buf, len, "%s: %s", ROW_LABEL[k - 1], TARGET_LABEL[in->map[k - 1]]);
    }
    if (on)
        return 0;
    if ((k = numbered(key, "name", MMD_STRIPS)))
        return snprintf(buf, len, "%s", s->strip[k - 1].present ? s->strip[k - 1].name : "");
    if ((k = numbered(key, "colour", MMD_STRIPS)))
        return snprintf(buf, len, "%d", s->strip[k - 1].present ? palette(s->strip[k - 1].colour) : COLOUR_NONE);
    if (!strcmp(key, "status"))
        return status(in, buf, len);
    if (!strcmp(key, "hint"))
        return hint(in, buf, len);
    if (!strcmp(key, "edit_title"))
        return snprintf(buf, len, "%s", ROW_TITLE[in->row]);
    if (!strcmp(key, "edit_help"))
        return help(in, buf, len);
    if (!strcmp(key, "rowkind"))
        return snprintf(buf, len, "%d", in->row >= MAP_LEVEL_ROWS);
    if (!strcmp(key, "edit_level"))
        return snprintf(buf, len, "%d", in->row < MAP_LEVEL_ROWS ? index_of(map_level_targets, MAP_LEVEL_TARGETS,
                                                                            in->map[in->row]) : 0);
    if (!strcmp(key, "edit_button"))
        return snprintf(buf, len, "%d", in->row >= MAP_LEVEL_ROWS ? index_of(map_button_targets, MAP_BUTTON_TARGETS,
                                                                             in->map[in->row]) : 0);
    return 0;   /* *_display: the wrapper's own text */
}

static void set_param(void *p, const char *key, const char *val)
{
    inst_t *in = p;
#ifdef ENGINE_TRACE   /* a debug build: what the host sets, to /tmp */
    FILE *f = fopen("/tmp/midimix-editor.trace", "a");
    if (f) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        fprintf(f, "%ld.%03ld %p set %s = %s\n", (long)t.tv_sec, t.tv_nsec / 1000000, p, key, val);
        fclose(f);
    }
#endif
    unsigned char m[MAP_ROWS];
    char err[96];
    int v = atoi(val), k;
    memcpy(m, in->map, sizeof m);
    if (!strcmp(key, "state")) {   /* a project's mapping; rows it doesn't name take the default's */
        memcpy(m, map_default, sizeof m);
        if (!map_parse(m, val, err, sizeof err))
            set_map(in, m);
    } else if (!strcmp(key, "edit_level") && in->row < MAP_LEVEL_ROWS && v >= 0 && v < MAP_LEVEL_TARGETS) {
        m[in->row] = map_level_targets[v];
        set_map(in, m);
    } else if (!strcmp(key, "edit_button") && in->row >= MAP_LEVEL_ROWS && v >= 0 && v < MAP_BUTTON_TARGETS) {
        m[in->row] = map_button_targets[v];
        set_map(in, m);
    } else if ((k = numbered(key, "row", MAP_ROWS)) && in->row != k - 1) {   /* a tap on a row ("0" on the lit one) */
        in->row = k - 1;
        in->rev++;
    }
}

const mpc_engine_t *mpc_engine(void)
{
    static const mpc_engine_t api = {create, destroy, midi, set_param, get_param, render, NULL, NULL};
    return &api;
}
