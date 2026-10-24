// midimixd: the Akai Midimix as a control surface for MPC 3.9.1.2 on the Key 37. Milestone 2: the
// LEDs. 30 times a second it reads every track's mute / solo / rec arm from MPC's memory (mpcread:
// read-only) and lights the Midimix to match; bank left / right page its 8 strips through MPC's
// tracks. The other buttons, the knobs and the faders do nothing yet: orders to MPC come next.
//
//   midimixd [-v]        -v: also log every LED message and what the Midimix sends
//
// LEDs: mute steady = muted, blinking = silenced by another track's solo; solo = soloed (the
// Midimix shows the solo LEDs on the mute row while SOLO is held, by itself); rec arm = armed;
// bank left / right = there are tracks that way. All off while MPC can't be read, and on exit.
// The Midimix is found by its sequencer client's name and followed through replugs; MPC is
// followed through restarts.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asequencer.h>

#include "../mpcread/mpcread.h"

#define MIDIMIX_NAME    "MIDI Mix"  // its ALSA sequencer client
#define STRIPS          8
#define NOTE_BANK_LEFT  25
#define NOTE_BANK_RIGHT 26
#define LEDS            27  // notes 1-26 have an LED (strip n: mute 3n+1, solo 3n+2, arm 3n+3)
#define ON              127

#define POLL_HZ   30
#define BLINK_S   0.25  // half a blink: 2 Hz, as MPC blinks its pads
#define FAILS_OFF 3     // bad reads in a row before the LEDs go off

static int verbose;
static double t_start;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...) {
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

// --- MPC's state ---

static struct mpc_state st;  // the last good read
static int known;            // st is current: the LEDs show it
static int failing;          // bad reads in a row
static const char *problem;  // last thing logged about MPC, to log each only once
static double next_attach, scan_wait = 2;
static pid_t scanned_pid;
static int bank;

static void set_bank(int b, const char *why) {
    bank = b;
    say("bank %d: tracks %d-%d%s", bank + 1, bank * STRIPS + 1, bank * STRIPS + STRIPS, why);
}

static void mpc_problem(const char *why) {
    if (why != problem) say("MPC: %s", why);
    problem = why;
}

static void mpc_tick(double t) {
    static struct mpc_state s;
    if (!mpc_project) {
        if (t < next_attach) return;
        struct mpc_found f;
        const char *why = mpc_attach(&f);
        if (why) {
            mpc_problem(why);
            next_attach = t + 2;
            if (f.where) {  // MPC is there but has no Project yet (starting): back off the scans
                scan_wait = f.pid == scanned_pid && scan_wait < 30 ? scan_wait * 2 : 2;
                scanned_pid = f.pid;
                next_attach = t + scan_wait;
            }
            return;
        }
        say("MPC pid %d: Project at 0x%x (%.0f MB searched in %.0f ms)", f.pid, f.project, f.mb, f.ms);
        scanned_pid = 0;
        problem = NULL;
        failing = 0;
    }

    const char *why = mpc_poll(&s);
    if (!why) {
        if (!known && problem) say("MPC: readable again, %d tracks", s.n);
        st = s;
        known = 1;
        failing = 0;
        problem = NULL;
        if (bank > 0 && bank * STRIPS >= st.n) set_bank(st.n ? (st.n - 1) / STRIPS : 0, " (tracks deleted)");
        return;
    }
    if (++failing < FAILS_OFF) return;  // a torn read, or MPC in the middle of a change
    known = 0;
    mpc_problem(why);
    if (!mpc_project_alive()) {  // MPC restarting, or its Project replaced: search again
        mpc_project = 0;
        next_attach = t;
    }
}

// --- the LEDs ---

static int blink_on;

static void leds_flush(void) {
    unsigned char want[LEDS] = {0};
    if (!midimix_up) return;
    if (known) {
        for (int n = 0; n < STRIPS; n++) {
            int t = bank * STRIPS + n;
            if (t >= st.n) break;
            const struct mpc_track *tr = &st.t[t];
            want[3 * n + 1] = tr->mute || (tr->silenced && blink_on) ? ON : 0;
            want[3 * n + 2] = tr->solo ? ON : 0;
            want[3 * n + 3] = tr->arm ? ON : 0;
        }
        want[NOTE_BANK_LEFT] = bank > 0 ? ON : 0;
        want[NOTE_BANK_RIGHT] = (bank + 1) * STRIPS < st.n ? ON : 0;
    }
    for (int note = 1; note < LEDS && midimix_up; note++) {
        if (sent[note] == want[note]) continue;
        send_note(note, want[note]);
        sent[note] = (signed char)want[note];
        if (verbose) say("led %2d %s", note, want[note] ? "on" : "off");
    }
}

// --- what arrives: the Midimix's buttons, clients coming and going ---

static void handle(const struct snd_seq_event *ev) {
    switch (ev->type) {
    case SNDRV_SEQ_EVENT_NOTEON:
        if (verbose) say("in note %d vel %d", ev->data.note.note, ev->data.note.velocity);
        if (!ev->data.note.velocity) break;
        if (ev->data.note.note == NOTE_BANK_LEFT && bank > 0) set_bank(bank - 1, "");
        if (ev->data.note.note == NOTE_BANK_RIGHT && known && (bank + 1) * STRIPS < st.n) set_bank(bank + 1, "");
        break;
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
    default:
        if (verbose && ev->type == SNDRV_SEQ_EVENT_CONTROLLER)
            say("in cc %u = %d", ev->data.control.param, ev->data.control.value);
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
    t_start = now();
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    seq_open();
    say("midimixd: seq client %d:%d", my_client, my_port);
    if (!find_midimix()) say("waiting for the Midimix (\"%s\")", MIDIMIX_NAME);

    double next_poll = now(), next_blink = now();
    while (!stop) {
        double t = now();
        if (t >= next_poll) {
            mpc_tick(t);
            next_poll += 1.0 / POLL_HZ;
            if (next_poll < now()) next_poll = now() + 1.0 / POLL_HZ;  // after a long scan
        }
        if (t >= next_blink) {
            blink_on = !blink_on;
            next_blink += BLINK_S;
            if (next_blink < t) next_blink = t + BLINK_S;
        }
        leds_flush();
        double wake = next_poll < next_blink ? next_poll : next_blink;
        int ms = (int)((wake - now()) * 1000) + 1;
        struct pollfd pfd = {.fd = seq_fd, .events = POLLIN};
        if (poll(&pfd, 1, ms > 0 ? ms : 0) > 0) read_events();
    }

    known = 0;  // leave the Midimix dark rather than showing a state that will go stale
    leds_flush();
    say("stopped");
    return 0;
}
