// midimixd: the Akai Midimix as a control surface for MPC 3.9.1.2 on the Key 37, through mpc-tests'
// native API (api.c). The API's watch says what every track's mute / solo / rec arm / volume / pan
// is; the Midimix's LEDs follow it, and its buttons, faders and bottom knobs become API commands.
//
//   midimixd [-v]        -v: also log every LED message, what the Midimix sends and the API traffic
//
// Exits with status 3 when another midimixd is running (the systemd unit, see install.sh).
//
// Strips: 8 tracks at a time in MPC's Track List order; bank left / right page by 8.
// LEDs: mute steady = muted, blinking = silenced by another track's solo; solo = soloed (the Midimix
// shows the solo LEDs on the mute row while SOLO is held, by itself); rec arm = armed; bank left /
// right = there are tracks that way. All off while the API can't be read, and on exit.
// Buttons: mute / solo / rec arm toggle the strip's track. The LED shows the press at once; a press
// MPC refuses flashes three times.
// Faders: the strip's track volume; bottom knobs (above mute): its pan, CC 64 = centre. Each once it
// reaches the track's level (pickup). A change of that level on MPC's side lets go of the control
// until it reaches the new level. One volume or pan command at a time, at most 10 a second: a moving
// control sends its latest position.
// The other knobs and the master fader do nothing yet.
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

#define MIDIMIX_NAME    "MIDI Mix"  // its ALSA sequencer client
#ifndef LOCK_FILE  // a test build can point elsewhere
#define LOCK_FILE       "/run/midimixd.lock"
#endif
#define STRIPS          8
#define NOTE_BANK_LEFT  25
#define NOTE_BANK_RIGHT 26
#define LEDS            27  // notes 1-26 have an LED (strip n: mute 3n+1, solo 3n+2, arm 3n+3)
#define ON              127

static const char *const KIND[] = {"mute", "solo", "arm"};  // a strip's buttons, as api_op

// A strip's mixer controls: the fader sets its track's volume, the bottom knob its pan.
enum { VOLUME, PAN, CTL_KINDS };
static const unsigned char CTL_CC[CTL_KINDS][STRIPS] = {
    {19, 23, 27, 31, 49, 53, 57, 61},
    {18, 22, 26, 30, 48, 52, 56, 60},
};
static const char *const CTL[] = {"fader", "pan knob"};
static const char *const LEVEL[] = {"volume", "pan"};
static const enum api_op CTL_OP[] = {API_VOLUME, API_PAN};

#define TICK_MS    20
#define BLINK_S    0.25  // half a blink: 2 Hz, as MPC blinks its pads
#define FLASH_S    0.08  // half a refused press's flash
#define FLASHES    3
#define CONFIRM_S  0.6   // an applied press outranks the watch (sampled every 100 ms) this long
#define MIXER_GAP  0.1   // between volume / pan commands
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

// The watch's value of a strip button's flag: 1, 0, -1 unknown.
static int flag(const struct api_track *tr, int kind) {
    return kind == API_MUTE ? tr->mute : kind == API_SOLO ? tr->solo : tr->arm;
}

// The note of track t's button of this kind, or 0 when the track isn't in the current bank.
static int note_of(int t, int kind) {
    return t >= bank * STRIPS && t < (bank + 1) * STRIPS ? 3 * (t - bank * STRIPS) + 1 + kind : 0;
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
    int kind, value, id;
    double until;
} presses[32];

static double flash_until[LEDS];

static struct press *find_press(const char *track, int kind) {
    for (size_t i = 0; i < sizeof presses / sizeof *presses; i++)
        if (presses[i].track[0] && presses[i].kind == kind && !strcmp(presses[i].track, track)) return &presses[i];
    return NULL;
}

// What a strip button's LED shows: the press in progress, else the watch.
static int shown(const struct api_track *tr, int kind) {
    const struct press *p = find_press(tr->id, kind);
    return p ? p->value : flag(tr, kind);
}

static void flash(int note) {
    if (note > 0) flash_until[note] = now() + 2 * FLASHES * FLASH_S;
}

static void press(int note) {
    int strip = (note - 1) / 3, kind = (note - 1) % 3, t = bank * STRIPS + strip;
    if (!api.live || t >= api.n) return;
    const struct api_track *tr = &api.t[t];
    int cur = shown(tr, kind);
    struct press *p = find_press(tr->id, kind);
    for (size_t i = 0; !p && i < sizeof presses / sizeof *presses; i++)
        if (!presses[i].track[0]) p = &presses[i];
    if (cur < 0 || !p) {
        say("%s track %d (%s): %s", KIND[kind], t + 1, tr->name, cur < 0 ? "not readable on this track" : "busy");
        flash(note);
        return;
    }
    // The opposite of what the LED shows, if MPC still has that when the command runs.
    int id = api_send((enum api_op)kind, t, !cur, cur);
    if (id < 0) {
        flash(note);
        return;
    }
    snprintf(p->track, sizeof p->track, "%s", tr->id);
    p->kind = kind;
    p->value = !cur;
    p->id = id;
    say("%s track %d (%s) %s", KIND[kind], t + 1, tr->name, cur ? "off" : "on");
}

static void press_result(const struct api_result *r) {
    struct press *p = NULL;
    for (size_t i = 0; i < sizeof presses / sizeof *presses && !p; i++)
        if (presses[i].track[0] && presses[i].id == r->id) p = &presses[i];
    int t = track_index(r->track), applied = !strcmp(r->status, "applied");
    say("%s track %d: %s%s%s (%d ms)", KIND[r->op], t + 1, r->status, r->error ? " " : "", r->error ? r->error : "",
        r->total_ms);
    if (applied && p) {
        p->id = 0;
        p->until = now() + CONFIRM_S;
        return;
    }
    if (p) p->track[0] = '\0';  // the watch has the last word
    // unknown or accepted: it may have happened, the watch will tell; otherwise MPC said no.
    if (strcmp(r->status, "unknown") && strcmp(r->status, "accepted") && t >= 0) flash(note_of(t, r->op));
}

static void presses_tick(double t) {
    for (size_t i = 0; i < sizeof presses / sizeof *presses; i++) {
        struct press *p = &presses[i];
        if (!p->track[0] || p->id) continue;
        int k = track_index(p->track);
        if (k < 0 || t > p->until || flag(&api.t[k], p->kind) == p->value) p->track[0] = '\0';
    }
}

// --- faders and pan knobs ---

static struct ctl {
    int cc;                  // its last position 0-127, -1 not known yet
    int caught;              // it sets its track's level
    char track[API_ID_LEN];  // the track it caught
    double want;             // a level to send, -1 none
    double sent, sent_at;    // the last level sent (or found when catching)
} ctls[CTL_KINDS][STRIPS];

static int mixer_id, mixer_kind, mixer_strip, mixer_last;  // the volume / pan command out, if any
static double mixer_next;

static double level_of(const struct api_track *tr, int kind) { return kind == VOLUME ? tr->volume : tr->pan; }

// A level on the control's 0-127 scale, and back. Volume: CC / 127. Pan: CC / 128, so that CC 64 is
// the centre (0.5); 127 is hard right (1).
static double position(int kind, double lv) { return kind == VOLUME ? lv * 127 : fmin(lv * 128, 127); }
static double level(int kind, int cc) { return kind == VOLUME ? cc / 127.0 : cc == 127 ? 1 : cc / 128.0; }

static void release(int kind, int n, const char *why) {
    struct ctl *c = &ctls[kind][n];
    if (c->caught && why) say("%s %d lets go: %s", CTL[kind], n + 1, why);
    c->caught = 0;
    c->want = -1;
}

static void ctls_release_all(void) {
    for (int kind = 0; kind < CTL_KINDS; kind++)
        for (int n = 0; n < STRIPS; n++) release(kind, n, NULL);
}

static void ctl_move(int kind, int n, int v) {
    struct ctl *c = &ctls[kind][n];
    int prev = c->cc, t = bank * STRIPS + n;
    c->cc = v;
    if (!api.live || t >= api.n) return;
    const struct api_track *tr = &api.t[t];
    if (!c->caught) {
        double lv = level_of(tr, kind);
        if (lv < 0 || !tr->mixer_gen[0]) return;
        // Pickup: within a step of MPC's level, or passing it since the last message.
        double at = position(kind, lv);
        if (fabs(v - at) > 1 && !(prev >= 0 && (prev - at) * (v - at) < 0)) return;
        c->caught = 1;
        snprintf(c->track, sizeof c->track, "%s", tr->id);
        c->sent = lv;
        c->sent_at = now();
        say("%s %d picks up track %d (%s) at %.0f", CTL[kind], n + 1, t + 1, tr->name, at);
    }
    c->want = level(kind, v);
}

static void ctls_tick(double t) {
    for (int kind = 0; kind < CTL_KINDS; kind++)
        for (int n = 0; n < STRIPS; n++) {
            struct ctl *c = &ctls[kind][n];
            int k = bank * STRIPS + n;
            if (!c->caught) continue;
            if (!api.live || k >= api.n || strcmp(api.t[k].id, c->track)) {
                release(kind, n, api.live ? "its track is gone" : "no current state from MPC");
                continue;
            }
            if (c->want < 0 && !(mixer_id && mixer_kind == kind && mixer_strip == n) && t - c->sent_at > RELEASE_S &&
                fabs(position(kind, level_of(&api.t[k], kind)) - position(kind, c->sent)) > 1.5)
                release(kind, n, kind == VOLUME ? "the volume changed on MPC" : "the pan changed on MPC");
        }
    if (mixer_id || t < mixer_next) return;
    for (int i = 1; i <= CTL_KINDS * STRIPS; i++) {  // round robin, from the control after the last one sent
        int j = (mixer_last + i) % (CTL_KINDS * STRIPS), kind = j / STRIPS, n = j % STRIPS;
        struct ctl *c = &ctls[kind][n];
        if (!c->caught || c->want < 0) continue;
        int id = api_send(CTL_OP[kind], bank * STRIPS + n, c->want, -1);
        if (id < 0) {
            release(kind, n, "can't send");
            continue;
        }
        mixer_id = id;
        mixer_kind = kind;
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
    if (verbose) say("%s track %d: %s (%d ms)", LEVEL[mixer_kind], track_index(r->track) + 1, r->status, r->total_ms);
    // unknown: the level may be back under MPC's automation already; the watch will tell. A refusal
    // releases the control only if it still holds that track (not after a bank change, say).
    const struct ctl *c = &ctls[mixer_kind][mixer_strip];
    if (strcmp(r->status, "applied") && strcmp(r->status, "unknown") && strcmp(r->status, "accepted") && c->caught &&
        !strcmp(c->track, r->track))
        release(mixer_kind, mixer_strip, r->error ? r->error : r->status);
}

static void on_result(const struct api_result *r) {
    if (r->op == API_VOLUME || r->op == API_PAN) mixer_result(r);
    else press_result(r);
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
            const struct press *pm = find_press(tr->id, API_MUTE);
            int mute = shown(tr, API_MUTE) == 1;
            int silenced = !pm && !mute && tr->silenced == 1;  // by another track's solo
            want[3 * n + 1] = mute || (silenced && blink_on) ? ON : 0;
            want[3 * n + 2] = shown(tr, API_SOLO) == 1 ? ON : 0;
            want[3 * n + 3] = shown(tr, API_ARM) == 1 ? ON : 0;
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
        if (note >= 1 && note <= 3 * STRIPS) press(note);
        if (note == NOTE_BANK_LEFT && bank > 0) set_bank(bank - 1, "");
        if (note == NOTE_BANK_RIGHT && api.live && (bank + 1) * STRIPS < api.n) set_bank(bank + 1, "");
        break;
    }
    case SNDRV_SEQ_EVENT_CONTROLLER: {
        unsigned param = ev->data.control.param;
        int value = ev->data.control.value;
        if (verbose) say("in cc %u = %d", param, value);
        if (ev->data.control.channel != 0 || value < 0 || value > 127) break;
        for (int kind = 0; kind < CTL_KINDS; kind++)
            for (int n = 0; n < STRIPS; n++)
                if (param == CTL_CC[kind][n]) ctl_move(kind, n, value);
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
    for (int kind = 0; kind < CTL_KINDS; kind++)
        for (int n = 0; n < STRIPS; n++) ctls[kind][n] = (struct ctl){.cc = -1, .want = -1};

    seq_open();
    api_init(on_result);
    say("midimixd: seq client %d:%d", my_client, my_port);
    if (!find_midimix()) say("waiting for the Midimix (\"%s\")", MIDIMIX_NAME);

    double next_blink = now();
    while (!stop) {
        double t = now();
        api_tick(t);
        presses_tick(t);
        if (bank > 0 && api.live && bank * STRIPS >= api.n)
            set_bank(api.n ? (api.n - 1) / STRIPS : 0, " (tracks deleted)");
        ctls_tick(t);
        if (t >= next_blink) {
            blink_on = !blink_on;
            next_blink += BLINK_S;
            if (next_blink < t) next_blink = t + BLINK_S;
        }
        leds_flush(t);

        struct pollfd fds[1 + API_MAX_FDS] = {{.fd = seq_fd, .events = POLLIN}};
        int n = 1 + api_pollfds(fds + 1, API_MAX_FDS);
        if (poll(fds, (nfds_t)n, TICK_MS) > 0) {
            if (fds[0].revents) read_events();
            api_events(fds + 1, n - 1);
        }
    }

    api.live = 0;  // leave the Midimix dark rather than showing a state that will go stale
    memset(flash_until, 0, sizeof flash_until);
    leds_flush(now());
    api_close();
    say("stopped");
    return 0;
}
