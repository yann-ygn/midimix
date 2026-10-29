// midimixd: the Akai Midimix as a control surface for MPC 3.9.1.2 on the Key 37, through mpc-tests'
// native API (api.c). The API's watch says what every track's mute / solo / rec arm / mixer is; the
// Midimix's LEDs follow it, and its buttons, faders and knobs become API commands.
//
//   midimixd [-v]        -v: also log every LED message, what the Midimix sends and the API traffic
//
// Exits with status 3 when another midimixd is running (the systemd unit, see install.sh).
//
// Strips: 8 tracks at a time in MPC's Track List order; bank left / right page by 8.
// Mapping (map.h): what each row of the strips does, the same for the 8 strips. By default the faders
// set the volume, the bottom knobs (above mute) the pan, the buttons mute / solo / rec arm, and the top
// two knobs nothing. The editor plugin replaces it through the control socket (ctl.h) while it's loaded.
// LEDs: a button's LED shows its target on the strip's track: mute steady = muted, blinking = silenced
// by another track's solo; solo, rec arm, an insert slot = on; select = the selected track (the Midimix
// shows the solo row's LEDs on the mute row while SOLO is held, by itself). Bank left / right = there
// are tracks that way. All off while the API can't be read, and on exit.
// Buttons: toggle their target on the strip's track (select: select it). The LED shows the press at
// once; a press MPC refuses flashes three times.
// Knobs and faders: their level (volume, pan, a send) on the strip's track, once they reach it
// (pickup). Pan: CC 64 = centre. A change of that level on MPC's side lets go of the control until it
// reaches the new level. One level command at a time, at most 10 a second: a moving control sends its
// latest position. The master fader does nothing.
// The Midimix is found by its sequencer client's name and followed through replugs; the API is
// followed through restarts (MPC's, or the addin's).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asequencer.h>

#include "api.h"
#include "ctl.h"
#include "map.h"

#define MIDIMIX_NAME    "MIDI Mix"  // its ALSA sequencer client
#ifndef LOCK_FILE  // a test build can point elsewhere
#define LOCK_FILE       "/run/midimixd.lock"
#endif
#define STRIPS          8
#define NOTE_BANK_LEFT  25
#define NOTE_BANK_RIGHT 26
#define LEDS            27  // notes 1-26 have an LED (strip n: mute 3n+1, solo 3n+2, arm 3n+3)
#define ON              127

// The level rows' CCs (map.h's ROW_TOP to ROW_FADER). Button row ROW_MUTE + k of strip n is note 3n + 1 + k.
static const unsigned char LEVEL_CC[MAP_LEVEL_ROWS][STRIPS] = {
    {16, 20, 24, 28, 46, 50, 54, 58},
    {17, 21, 25, 29, 47, 51, 55, 59},
    {18, 22, 26, 30, 48, 52, 56, 60},
    {19, 23, 27, 31, 49, 53, 57, 61},
};
static const char *const ROW[MAP_ROWS] = {"top knob", "middle knob", "bottom knob", "fader", "mute", "solo", "rec arm"};

static unsigned char map[MAP_ROWS];  // the mapping in effect (ctl_map)

// The last control touched, for the editor: it picks that row. A moving control's messages are one
// touch; the same control again after TOUCH_S, or a button press, is a new one.
#define TOUCH_S 0.5
static struct {
    unsigned seq;
    int row, strip;
    double at;
} touched;

static void touch(int row, int strip) {
    double t = now();
    int same = touched.seq && touched.row == row && touched.strip == strip && row < MAP_LEVEL_ROWS && t - touched.at < TOUCH_S;
    touched.at = t;
    if (same) return;
    touched.seq++;
    touched.row = row;
    touched.strip = strip;
}

#define TICK_MS    20
#define BLINK_S    0.25  // half a blink: 2 Hz, as MPC blinks its pads
#define FLASH_S    0.08  // half a refused press's flash
#define FLASHES    3
#define CONFIRM_S  0.6   // an applied press outranks the watch (sampled every 100 ms) this long
#define MIXER_GAP  0.1   // between level commands
#define RELEASE_S  0.5   // after the last level sent: MPC's level must be that one, or let go

int verbose;
static double t_start;

double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%9.3f ", now() - t_start);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

static void die(const char *what) {
    fprintf(stderr, "midimixd: %s: %s\n", what, strerror(errno));
    exit(1);
}

// --- the Midimix, through the ALSA sequencer (kernel ioctls, no libasound) ---

static int seq_fd = -1, my_client, my_port;
static struct snd_seq_addr midimix;
static int midimix_up;
static signed char sent[LEDS];  // last velocity sent per note: 0, ON, or -1 not known

static void seq_open(void) {
    seq_fd = open("/dev/snd/seq", O_RDWR | O_NONBLOCK);
    if (seq_fd < 0) die("open /dev/snd/seq");
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &my_client) < 0) die("client id");

    struct snd_seq_client_info info = {.client = my_client};
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_GET_CLIENT_INFO, &info) < 0) die("get client info");
    snprintf(info.name, sizeof info.name, "midimixd");
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &info) < 0) die("set client info");

    // Write only, no SUBS_* capability: MPC lists neither an input nor an output for it.
    struct snd_seq_port_info port = {.addr = {.client = (unsigned char)my_client}};
    snprintf(port.name, sizeof port.name, "Midimix");
    port.capability = SNDRV_SEQ_PORT_CAP_WRITE;
    port.type = SNDRV_SEQ_PORT_TYPE_APPLICATION;
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &port) < 0) die("create port");
    my_port = port.addr.port;

    // Clients and ports coming and going, to follow the Midimix through replugs.
    struct snd_seq_port_subscribe sub = {
        .sender = {.client = SNDRV_SEQ_CLIENT_SYSTEM, .port = SNDRV_SEQ_PORT_SYSTEM_ANNOUNCE},
        .dest = {.client = (unsigned char)my_client, .port = (unsigned char)my_port},
    };
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) < 0) die("subscribe to announcements");
}

// Looks for the Midimix by name and subscribes to what it sends. 1 when connected.
static int find_midimix(void) {
    struct snd_seq_client_info c = {.client = -1};
    while (ioctl(seq_fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT, &c) == 0) {
        if (strcmp(c.name, MIDIMIX_NAME)) continue;
        // port 255 wraps to 0 in the kernel's "next port" search: its first port
        struct snd_seq_port_info p = {.addr = {.client = (unsigned char)c.client, .port = 255}};
        if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT, &p) < 0) continue;  // no port yet
        struct snd_seq_port_subscribe sub = {
            .sender = p.addr, .dest = {.client = (unsigned char)my_client, .port = (unsigned char)my_port}};
        if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) < 0 && errno != EBUSY) {
            say("Midimix at %d:%d, but subscribing failed: %s", p.addr.client, p.addr.port, strerror(errno));
            return 0;
        }
        // And to it: the kernel only keeps its MIDI output open while it has a subscriber. Without
        // ours, every LED message failed (ENODEV) until MPC subscribed, at boot (2026-10-28).
        struct snd_seq_port_subscribe out = {.sender = sub.dest, .dest = p.addr};
        if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &out) < 0 && errno != EBUSY) {
            say("Midimix at %d:%d, but subscribing to it failed: %s", p.addr.client, p.addr.port, strerror(errno));
            return 0;
        }
        midimix = p.addr;
        midimix_up = 1;
        memset(sent, -1, sizeof sent);  // whatever it shows now: send every LED
        say("Midimix at %d:%d", midimix.client, midimix.port);
        return 1;
    }
    return 0;
}

static void send_note(int note, int velocity) {
    struct snd_seq_event ev = {0};
    ev.type = SNDRV_SEQ_EVENT_NOTEON;
    ev.queue = SNDRV_SEQ_QUEUE_DIRECT;
    ev.source.client = (unsigned char)my_client;
    ev.source.port = (unsigned char)my_port;
    ev.dest = midimix;
    ev.data.note.channel = 0;
    ev.data.note.note = (unsigned char)note;
    ev.data.note.velocity = (unsigned char)velocity;
    if (write(seq_fd, &ev, sizeof ev) != sizeof ev) {
        say("Midimix: sending failed: %s", strerror(errno));
        midimix_up = 0;
    }
}

// --- tracks and banks ---

static int bank;

static int track_index(const char *id) {
    for (int t = 0; t < api.n; t++)
        if (!strcmp(api.t[t].id, id)) return t;
    return -1;
}

// The watch's value of a button target on a track: 1, 0, -1 unknown (or an empty insert slot).
static int flag(const struct api_track *tr, int target) {
    switch (target) {
    case T_MUTE: return tr->mute;
    case T_SOLO: return tr->solo;
    case T_ARM: return tr->arm;
    case T_SELECT: return !strcmp(api.selected, tr->id);
    default: return target >= T_INSERT1 && target <= T_INSERT4 ? tr->insert[target - T_INSERT1] : -1;
    }
}

// The note of a button row's button on strip n.
static int note_at(int row, int n) { return 3 * n + 1 + row - ROW_MUTE; }

// The note of track t's button in this row, or 0 when the track isn't in the current bank.
static int note_of(int t, int row) {
    return t >= bank * STRIPS && t < (bank + 1) * STRIPS ? note_at(row, t - bank * STRIPS) : 0;
}

static void ctls_release_all(void);

static void set_bank(int b, const char *why) {
    bank = b;
    ctls_release_all();  // their tracks changed
    say("bank %d: tracks %d-%d%s", bank + 1, bank * STRIPS + 1, bank * STRIPS + STRIPS, why);
}

// --- button presses ---

// A press waiting for its receipt (id > 0), or applied and shown until the watch agrees (id 0).
static struct press {
    char track[API_ID_LEN];  // "" for a free slot
    int target, row, value, id;
    double until;
} presses[32];

static double flash_until[LEDS];

static struct press *find_press(const char *track, int target) {
    for (size_t i = 0; i < sizeof presses / sizeof *presses; i++)
        if (presses[i].track[0] && presses[i].target == target && (!track || !strcmp(presses[i].track, track)))
            return &presses[i];
    return NULL;
}

// What a button's LED shows for its target on a track: the press in progress, else the watch. A select
// press moves the selection off the other tracks too.
static int shown(const struct api_track *tr, int target) {
    const struct press *p = find_press(target == T_SELECT ? NULL : tr->id, target);
    if (p && target == T_SELECT) return !strcmp(p->track, tr->id);
    return p ? p->value : flag(tr, target);
}

static void flash(int note) {
    if (note > 0) flash_until[note] = now() + 2 * FLASHES * FLASH_S;
}

static enum api_op button_op(int target) {
    switch (target) {
    case T_MUTE: return API_MUTE;
    case T_SOLO: return API_SOLO;
    case T_ARM: return API_ARM;
    case T_SELECT: return API_SELECT;
    default: return API_INSERT;
    }
}

static void press(int row, int strip) {
    int target = map[row], note = note_at(row, strip), t = bank * STRIPS + strip;
    touch(row, strip);
    if (target == T_NONE || !api.live || t >= api.n) return;
    const struct api_track *tr = &api.t[t];
    const char *what = map_target_name[target];
    int cur = shown(tr, target);
    if (target == T_SELECT && cur == 1) return;  // selected already
    struct press *p = find_press(target == T_SELECT ? NULL : tr->id, target);
    for (size_t i = 0; !p && i < sizeof presses / sizeof *presses; i++)
        if (!presses[i].track[0]) p = &presses[i];
    if (cur < 0 || !p) {
        say("%s track %d (%s): %s", what, t + 1, tr->name,
            !p                                            ? "busy"
            : target >= T_INSERT1 && target <= T_INSERT4 ? "no insert in that slot, or not readable"
                                                          : "not readable on this track");
        flash(note);
        return;
    }
    // The opposite of what the LED shows, if MPC still has that when the command runs.
    int id = api_send(button_op(target), t, target - T_INSERT1, !cur, target == T_SELECT ? -1 : cur);
    if (id < 0) {
        flash(note);
        return;
    }
    snprintf(p->track, sizeof p->track, "%s", tr->id);
    p->target = target;
    p->row = row;
    p->value = target == T_SELECT ? 1 : !cur;
    p->id = id;
    say("%s track %d (%s)%s", what, t + 1, tr->name, target == T_SELECT ? "" : cur ? " off" : " on");
}

static void press_result(const struct api_result *r) {
    struct press *p = NULL;
    for (size_t i = 0; i < sizeof presses / sizeof *presses && !p; i++)
        if (presses[i].track[0] && presses[i].id == r->id) p = &presses[i];
    int t = track_index(r->track), applied = !strcmp(r->status, "applied");
    say("%s track %d: %s%s%s (%d ms)", p ? map_target_name[p->target] : "press", t + 1, r->status, r->error ? " " : "",
        r->error ? r->error : "", r->total_ms);
    if (!p) return;  // another select press took over
    if (applied) {
        p->id = 0;
        p->until = now() + CONFIRM_S;
        return;
    }
    p->track[0] = '\0';  // the watch has the last word
    // unknown or accepted: it may have happened, the watch will tell; otherwise MPC said no.
    if (strcmp(r->status, "unknown") && strcmp(r->status, "accepted") && t >= 0 && map[p->row] == p->target)
        flash(note_of(t, p->row));
}

static void presses_tick(double t) {
    for (size_t i = 0; i < sizeof presses / sizeof *presses; i++) {
        struct press *p = &presses[i];
        if (!p->track[0] || p->id) continue;
        int k = track_index(p->track);
        if (k < 0 || t > p->until || flag(&api.t[k], p->target) == p->value) p->track[0] = '\0';
    }
}

// --- knobs and faders ---

static struct ctl {
    int cc;                  // its last position 0-127, -1 not known yet
    int caught;              // it sets its track's level
    char track[API_ID_LEN];  // the track it caught
    double want;             // a level to send, -1 none
    double sent, sent_at;    // the last level sent (or found when catching)
} ctls[MAP_LEVEL_ROWS][STRIPS];

static int mixer_id, mixer_row, mixer_strip, mixer_last;  // the level command out, if any
static double mixer_next;

static double level_of(const struct api_track *tr, int target) {
    if (target >= T_SEND1 && target <= T_SEND4) return tr->send[target - T_SEND1];
    return target == T_VOLUME ? tr->volume : target == T_PAN ? tr->pan : -1;
}

// A level on the control's 0-127 scale, and back. Pan: CC / 128, so that CC 64 is the centre (0.5);
// 127 is hard right (1). The others: CC / 127.
static double position(int target, double lv) { return target == T_PAN ? fmin(lv * 128, 127) : lv * 127; }
static double level(int target, int cc) { return target != T_PAN ? cc / 127.0 : cc == 127 ? 1 : cc / 128.0; }

static void release(int row, int n, const char *why) {
    struct ctl *c = &ctls[row][n];
    if (c->caught && why) say("%s %d lets go: %s", ROW[row], n + 1, why);
    c->caught = 0;
    c->want = -1;
}

static void ctls_release_all(void) {
    for (int row = 0; row < MAP_LEVEL_ROWS; row++)
        for (int n = 0; n < STRIPS; n++) release(row, n, NULL);
}

static void ctl_move(int row, int n, int v) {
    struct ctl *c = &ctls[row][n];
    int prev = c->cc, t = bank * STRIPS + n, target = map[row];
    c->cc = v;
    touch(row, n);
    if (target == T_NONE || !api.live || t >= api.n) return;
    const struct api_track *tr = &api.t[t];
    if (!c->caught) {
        double lv = level_of(tr, target);
        if (lv < 0 || !tr->mixer_gen[0]) return;
        // Pickup: within a step of MPC's level, or passing it since the last message.
        double at = position(target, lv);
        if (fabs(v - at) > 1 && !(prev >= 0 && (prev - at) * (v - at) < 0)) return;
        c->caught = 1;
        snprintf(c->track, sizeof c->track, "%s", tr->id);
        c->sent = lv;
        c->sent_at = now();
        say("%s %d picks up the %s of track %d (%s) at %.0f", ROW[row], n + 1, map_target_name[target], t + 1, tr->name, at);
    }
    c->want = level(target, v);
}

static void ctls_tick(double t) {
    for (int row = 0; row < MAP_LEVEL_ROWS; row++)
        for (int n = 0; n < STRIPS; n++) {
            struct ctl *c = &ctls[row][n];
            int k = bank * STRIPS + n, target = map[row];
            if (!c->caught) continue;
            if (!api.live || k >= api.n || strcmp(api.t[k].id, c->track)) {
                release(row, n, api.live ? "its track is gone" : "no current state from MPC");
                continue;
            }
            if (c->want < 0 && !(mixer_id && mixer_row == row && mixer_strip == n) && t - c->sent_at > RELEASE_S &&
                fabs(position(target, level_of(&api.t[k], target)) - position(target, c->sent)) > 1.5) {
                char why[48];
                snprintf(why, sizeof why, "the %s changed on MPC", map_target_name[target]);
                release(row, n, why);
            }
        }
    if (mixer_id || t < mixer_next) return;
    for (int i = 1; i <= MAP_LEVEL_ROWS * STRIPS; i++) {  // round robin, from the control after the last one sent
        int j = (mixer_last + i) % (MAP_LEVEL_ROWS * STRIPS), row = j / STRIPS, n = j % STRIPS, target = map[row];
        struct ctl *c = &ctls[row][n];
        if (!c->caught || c->want < 0) continue;
        enum api_op op = target == T_VOLUME ? API_VOLUME : target == T_PAN ? API_PAN : API_SEND;
        int id = api_send(op, bank * STRIPS + n, target - T_SEND1, c->want, -1);
        if (id < 0) {
            release(row, n, "can't send");
            continue;
        }
        mixer_id = id;
        mixer_row = row;
        mixer_strip = n;
        mixer_last = j;
        c->sent = c->want;
        c->sent_at = t;
        c->want = -1;
        mixer_next = t + MIXER_GAP;
        return;
    }
}

static void mixer_result(const struct api_result *r) {
    if (r->id != mixer_id) return;
    mixer_id = 0;
    if (verbose)
        say("%s track %d: %s (%d ms)", r->op == API_SEND ? "send" : r->op == API_PAN ? "pan" : "volume",
            track_index(r->track) + 1, r->status, r->total_ms);
    // unknown: the level may be back under MPC's automation already; the watch will tell. A refusal
    // releases the control only if it still holds that track (not after a bank change, say).
    const struct ctl *c = &ctls[mixer_row][mixer_strip];
    if (strcmp(r->status, "applied") && strcmp(r->status, "unknown") && strcmp(r->status, "accepted") && c->caught &&
        !strcmp(c->track, r->track))
        release(mixer_row, mixer_strip, r->error ? r->error : r->status);
}

static void on_result(const struct api_result *r) {
    if (r->op == API_VOLUME || r->op == API_PAN || r->op == API_SEND) mixer_result(r);
    else press_result(r);
}

// --- the mapping, from the control socket ---

static void map_follow(void) {
    int owner;
    const unsigned char *m = ctl_map(&owner);
    if (!memcmp(m, map, sizeof map)) return;
    memcpy(map, m, sizeof map);
    ctls_release_all();  // their targets changed
    char spec[MAP_SPEC_MAX];
    map_format(map, spec, sizeof spec);
    say("mapping %s: %s", owner < 0 ? "(default)" : "(editor)", spec);
}

// The state for the control socket's get (ctl.h). One "key values" line each:
//   midimixd 1                       the protocol's version
//   midimix 0|1                      the Midimix is there
//   api 0|1                          MPC's state is current
//   bank <n> <banks>                 the bank shown (from 1) and how many there are
//   tracks <n>
//   map <spec>                       the mapping in effect (map.h)
//   mapfrom you|other|default        whose: this connection's, another's, midimixd's default
//   touch <seq> <row> <strip>        the last control touched: seq counts touches (0: none yet)
//   selected <strip>                 the selected track's strip, 0 when it isn't in this bank
//   strip <n> <colour> <type> <name> for each strip: RGB24 in hex or "-", the track's type ("-": no
//                                    track), its name to the end of the line
static void addf(char *buf, size_t cap, size_t *n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = *n < cap ? vsnprintf(buf + *n, cap - *n, fmt, ap) : 0;
    va_end(ap);
    if (r > 0) *n = *n + (size_t)r < cap ? *n + (size_t)r : cap - 1;
}

size_t ctl_state(int c, char *buf, size_t cap) {
    size_t n = 0;
    int owner, banks = api.live && api.n ? (api.n + STRIPS - 1) / STRIPS : 1, sel = 0;
    ctl_map(&owner);
    char spec[MAP_SPEC_MAX];
    map_format(map, spec, sizeof spec);
    addf(buf, cap, &n, "midimixd 1\nmidimix %d\napi %d\nbank %d %d\ntracks %d\nmap %s\nmapfrom %s\n", midimix_up, api.live, bank + 1,
        banks, api.live ? api.n : 0, spec, owner == c ? "you" : owner >= 0 ? "other" : "default");
    addf(buf, cap, &n, "touch %u %s %d\n", touched.seq, map_row_name[touched.row], touched.strip + 1);
    for (int s = 0; s < STRIPS && api.live; s++) {
        int t = bank * STRIPS + s;
        if (t < api.n && api.selected[0] && !strcmp(api.t[t].id, api.selected)) sel = s + 1;
    }
    addf(buf, cap, &n, "selected %d\n", sel);
    for (int s = 0; s < STRIPS; s++) {
        int t = bank * STRIPS + s;
        if (!api.live || t >= api.n) {
            addf(buf, cap, &n, "strip %d - - \n", s + 1);
            continue;
        }
        const struct api_track *tr = &api.t[t];
        char name[sizeof tr->name], colour[20] = "-";
        snprintf(name, sizeof name, "%s", tr->name);
        for (char *p = name; *p; p++)
            if ((unsigned char)*p < ' ') *p = ' ';
        if (tr->colour >= 0) snprintf(colour, sizeof colour, "%06lx", tr->colour & 0xffffff);
        addf(buf, cap, &n, "strip %d %s %s %s\n", s + 1, colour, tr->type[0] ? tr->type : "?", name);
    }
    addf(buf, cap, &n, "\n");
    return n;
}

// --- the LEDs ---

static int blink_on;

static void leds_flush(double t) {
    unsigned char want[LEDS] = {0};
    if (!midimix_up) return;
    if (api.live) {
        for (int n = 0; n < STRIPS; n++) {
            int k = bank * STRIPS + n;
            if (k >= api.n) break;
            const struct api_track *tr = &api.t[k];
            for (int row = ROW_MUTE; row < MAP_ROWS; row++) {
                int target = map[row];
                if (target == T_NONE) continue;
                int on = shown(tr, target) == 1;
                if (target == T_MUTE && !on && !find_press(tr->id, T_MUTE) && tr->silenced == 1)
                    on = blink_on;  // silenced by another track's solo
                want[note_at(row, n)] = on ? ON : 0;
            }
        }
        want[NOTE_BANK_LEFT] = bank > 0 ? ON : 0;
        want[NOTE_BANK_RIGHT] = (bank + 1) * STRIPS < api.n ? ON : 0;
    }
    for (int note = 1; note < LEDS; note++)
        if (flash_until[note] > t) want[note] = (int)((flash_until[note] - t) / FLASH_S) % 2 ? ON : 0;
    for (int note = 1; note < LEDS && midimix_up; note++) {
        if (sent[note] == want[note]) continue;
        send_note(note, want[note]);
        sent[note] = (signed char)want[note];
        if (verbose) say("led %2d %s", note, want[note] ? "on" : "off");
    }
}

// --- what arrives: the Midimix's buttons and faders, clients coming and going ---

static void handle(const struct snd_seq_event *ev) {
    switch (ev->type) {
    case SNDRV_SEQ_EVENT_NOTEON: {
        int note = ev->data.note.note;
        if (verbose) say("in note %d vel %d", note, ev->data.note.velocity);
        if (!ev->data.note.velocity) break;
        if (note >= 1 && note <= 3 * STRIPS) press(ROW_MUTE + (note - 1) % 3, (note - 1) / 3);
        if (note == NOTE_BANK_LEFT && bank > 0) set_bank(bank - 1, "");
        if (note == NOTE_BANK_RIGHT && api.live && (bank + 1) * STRIPS < api.n) set_bank(bank + 1, "");
        break;
    }
    case SNDRV_SEQ_EVENT_CONTROLLER: {
        unsigned param = ev->data.control.param;
        int value = ev->data.control.value;
        if (verbose) say("in cc %u = %d", param, value);
        if (ev->data.control.channel != 0 || value < 0 || value > 127) break;
        for (int row = 0; row < MAP_LEVEL_ROWS; row++)
            for (int n = 0; n < STRIPS; n++)
                if (param == LEVEL_CC[row][n]) ctl_move(row, n, value);
        break;
    }
    case SNDRV_SEQ_EVENT_PORT_START:
        if (!midimix_up) find_midimix();
        break;
    case SNDRV_SEQ_EVENT_CLIENT_EXIT:
    case SNDRV_SEQ_EVENT_PORT_EXIT:
        if (midimix_up && ev->data.addr.client == midimix.client) {
            midimix_up = 0;
            say("Midimix gone");
        }
        break;
    }
}

static void read_events(void) {
    unsigned char buf[4096];
    ssize_t n;
    while ((n = read(seq_fd, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off + (ssize_t)sizeof(struct snd_seq_event) <= n;) {
            struct snd_seq_event ev;
            memcpy(&ev, buf + off, sizeof ev);
            off += sizeof ev;
            if ((ev.flags & SNDRV_SEQ_EVENT_LENGTH_MASK) == SNDRV_SEQ_EVENT_LENGTH_VARIABLE)
                off += ev.data.ext.len;  // SysEx and the like follow the event
            handle(&ev);
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) die("read");
}

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "-v")) {
        verbose = 1;
    } else if (argc != 1) {
        fprintf(stderr, "usage: midimixd [-v]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    // One at a time: two would fight over the LEDs, and every press would toggle twice.
    int lock = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock < 0) die("open " LOCK_FILE);
    if (flock(lock, LOCK_EX | LOCK_NB) < 0) {
        if (errno != EWOULDBLOCK) die("lock " LOCK_FILE);
        fprintf(stderr, "midimixd: already running (systemctl stop midimixd, or kill it)\n");
        return 3;
    }
    t_start = now();
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    for (int row = 0; row < MAP_LEVEL_ROWS; row++)
        for (int n = 0; n < STRIPS; n++) ctls[row][n] = (struct ctl){.cc = -1, .want = -1};
    memcpy(map, map_default, sizeof map);

    seq_open();
    api_init(on_result);
    ctl_init();
    say("midimixd: seq client %d:%d", my_client, my_port);
    if (!find_midimix()) say("waiting for the Midimix (\"%s\")", MIDIMIX_NAME);

    double next_blink = now();
    while (!stop) {
        double t = now();
        api_tick(t);
        presses_tick(t);
        if (bank > 0 && api.live && bank * STRIPS >= api.n)
            set_bank(api.n ? (api.n - 1) / STRIPS : 0, " (tracks deleted)");
        map_follow();
        ctls_tick(t);
        if (t >= next_blink) {
            blink_on = !blink_on;
            next_blink += BLINK_S;
            if (next_blink < t) next_blink = t + BLINK_S;
        }
        leds_flush(t);

        struct pollfd fds[1 + API_MAX_FDS + CTL_MAX_FDS] = {{.fd = seq_fd, .events = POLLIN}};
        int na = api_pollfds(fds + 1, API_MAX_FDS), nc = ctl_pollfds(fds + 1 + na, CTL_MAX_FDS);
        if (poll(fds, (nfds_t)(1 + na + nc), TICK_MS) > 0) {
            if (fds[0].revents) read_events();
            api_events(fds + 1, na);
            ctl_events(fds + 1 + na, nc);
        }
    }

    api.live = 0;  // leave the Midimix dark rather than showing a state that will go stale
    memset(flash_until, 0, sizeof flash_until);
    leds_flush(now());
    api_close();
    ctl_close();
    say("stopped");
    return 0;
}
