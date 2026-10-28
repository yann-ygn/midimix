// seqtool: list ALSA sequencer ports, send note / CC messages to one, or print what one sends.
// Talks to the kernel's /dev/snd/seq directly (no libasound), so it builds as one static binary.
//
//   seqtool ports
//   seqtool send <client>:<port> <status> <data1> <data2> [...]   hex bytes, 3 per message
//   seqtool dump <client>:<port> [seconds]
//
// Only note on (9n), note off (8n) and control change (Bn) are sent: never SysEx (Akai's editor
// may rewrite the unit's mapping over SysEx).

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asequencer.h>

static int seq_fd = -1;
static int my_client, my_port;

static void die(const char *what) {
    fprintf(stderr, "seqtool: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void seq_open(void) {
    seq_fd = open("/dev/snd/seq", O_RDWR);
    if (seq_fd < 0) die("open /dev/snd/seq");
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &my_client) < 0) die("client id");

    struct snd_seq_client_info info = {.client = my_client};
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_GET_CLIENT_INFO, &info) < 0) die("get client info");
    snprintf(info.name, sizeof info.name, "seqtool");
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &info) < 0) die("set client info");

    struct snd_seq_port_info port = {.addr = {.client = my_client}};
    snprintf(port.name, sizeof port.name, "seqtool");
    port.capability = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_WRITE |
                      SNDRV_SEQ_PORT_CAP_SUBS_READ | SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
    port.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_APPLICATION;
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &port) < 0) die("create port");
    my_port = port.addr.port;
}

static void parse_addr(const char *s, struct snd_seq_addr *a) {
    int c, p;
    if (sscanf(s, "%d:%d", &c, &p) != 2 || c < 0 || c > 255 || p < 0 || p > 255) {
        fprintf(stderr, "seqtool: bad address '%s' (want client:port)\n", s);
        exit(2);
    }
    a->client = (unsigned char)c;
    a->port = (unsigned char)p;
}

static int cmd_ports(void) {
    struct snd_seq_client_info c = {.client = -1};
    while (ioctl(seq_fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT, &c) == 0) {
        printf("client %3d '%s'\n", c.client, c.name);
        // port 255 wraps to 0 in the kernel's "next port" search, as alsa-lib's -1 does
        struct snd_seq_port_info p = {.addr = {.client = (unsigned char)c.client, .port = 255}};
        while (ioctl(seq_fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT, &p) == 0)
            printf("  %3d:%-3d '%s'%s%s\n", c.client, p.addr.port, p.name,
                   (p.capability & SNDRV_SEQ_PORT_CAP_WRITE) ? " in" : "",
                   (p.capability & SNDRV_SEQ_PORT_CAP_READ) ? " out" : "");
    }
    return 0;
}

static int cmd_send(const struct snd_seq_addr *dest, int argc, char **argv) {
    if (argc == 0 || argc % 3) {
        fprintf(stderr, "seqtool: send takes whole messages, 3 hex bytes each\n");
        return 2;
    }
    for (int i = 0; i < argc; i += 3) {
        unsigned long b[3];
        for (int j = 0; j < 3; j++) {
            char *end;
            b[j] = strtoul(argv[i + j], &end, 16);
            if (*end || b[j] > 0xFF || (j && b[j] > 0x7F)) {
                fprintf(stderr, "seqtool: bad byte '%s'\n", argv[i + j]);
                return 2;
            }
        }
        struct snd_seq_event ev = {0};
        switch (b[0] & 0xF0) {
        case 0x90: ev.type = SNDRV_SEQ_EVENT_NOTEON; break;
        case 0x80: ev.type = SNDRV_SEQ_EVENT_NOTEOFF; break;
        case 0xB0: ev.type = SNDRV_SEQ_EVENT_CONTROLLER; break;
        default:
            fprintf(stderr, "seqtool: only 8n, 9n and Bn messages are sent\n");
            return 2;
        }
        ev.queue = SNDRV_SEQ_QUEUE_DIRECT;
        ev.source.client = (unsigned char)my_client;
        ev.source.port = (unsigned char)my_port;
        ev.dest = *dest;
        if (ev.type == SNDRV_SEQ_EVENT_CONTROLLER) {
            ev.data.control.channel = b[0] & 0x0F;
            ev.data.control.param = (unsigned)b[1];
            ev.data.control.value = (int)b[2];
        } else {
            ev.data.note.channel = b[0] & 0x0F;
            ev.data.note.note = (unsigned char)b[1];
            ev.data.note.velocity = (unsigned char)b[2];
        }
        if (write(seq_fd, &ev, sizeof ev) != sizeof ev) die("write event");
    }
    return 0;
}

static void print_event(const struct snd_seq_event *ev, double t) {
    switch (ev->type) {
    case SNDRV_SEQ_EVENT_NOTEON:
    case SNDRV_SEQ_EVENT_NOTEOFF:
        printf("%8.3f %d:%d ch%d note %s %3d vel %d\n", t, ev->source.client, ev->source.port,
               ev->data.note.channel + 1, ev->type == SNDRV_SEQ_EVENT_NOTEON ? "on " : "off",
               ev->data.note.note, ev->data.note.velocity);
        break;
    case SNDRV_SEQ_EVENT_CONTROLLER:
        printf("%8.3f %d:%d ch%d cc %3u = %d\n", t, ev->source.client, ev->source.port,
               ev->data.control.channel + 1, ev->data.control.param, ev->data.control.value);
        break;
    default:
        printf("%8.3f %d:%d event type %d\n", t, ev->source.client, ev->source.port, ev->type);
    }
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmd_dump(const struct snd_seq_addr *src, double secs) {
    struct snd_seq_port_subscribe sub = {.sender = *src};
    sub.dest.client = (unsigned char)my_client;
    sub.dest.port = (unsigned char)my_port;
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) < 0) die("subscribe");

    double start = now();
    unsigned char buf[4096];
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (;;) {
        double left = secs - (now() - start);
        if (secs > 0 && left <= 0) break;
        struct pollfd pfd = {.fd = seq_fd, .events = POLLIN};
        int r = poll(&pfd, 1, secs > 0 ? (int)(left * 1000) + 1 : -1);
        if (r < 0 && errno != EINTR) die("poll");
        if (r <= 0) continue;
        ssize_t n = read(seq_fd, buf, sizeof buf);
        if (n < 0) die("read");
        for (ssize_t off = 0; off + (ssize_t)sizeof(struct snd_seq_event) <= n;) {
            struct snd_seq_event ev;
            memcpy(&ev, buf + off, sizeof ev);
            off += sizeof ev;
            if ((ev.flags & SNDRV_SEQ_EVENT_LENGTH_MASK) == SNDRV_SEQ_EVENT_LENGTH_VARIABLE)
                off += ev.data.ext.len;  // SysEx and the like follow the event
            print_event(&ev, now() - start);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) goto usage;
    seq_open();
    if (!strcmp(argv[1], "ports")) return cmd_ports();
    if (argc >= 3) {
        struct snd_seq_addr a;
        parse_addr(argv[2], &a);
        if (!strcmp(argv[1], "send")) return cmd_send(&a, argc - 3, argv + 3);
        if (!strcmp(argv[1], "dump")) return cmd_dump(&a, argc > 3 ? atof(argv[3]) : 0);
    }
usage:
    fprintf(stderr, "usage: seqtool ports | send <client:port> <hex bytes> | dump <client:port> [secs]\n");
    return 2;
}
