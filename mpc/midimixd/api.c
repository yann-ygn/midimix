// api: see api.h. One request per connection, one JSON line back; the watch connection instead
// streams JSON lines (ack, snapshot, change, heartbeat, resync_required, terminated). A dropped
// watch resumes from the last applied frame's cursor; resync_required starts over without one.

#define _GNU_SOURCE
#include "api.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "json.h"

#define MAX_LINE   (2 << 20)  // frames and receipts are at most 1 MiB (the API's maxResponseBytes)
#define MAX_CMDS   (API_MAX_FDS - 1)
#define WAIT_MS    2000  // how long the API may hold a command's connection before its receipt
#define CMD_LIMIT  4     // seconds before giving up on a command's connection
#define RETRY_S    1     // between attempts to reach the API
#define SILENT_S   3.5   // the API sends at least a heartbeat a second
#define LOST_S     2     // after losing the watch, how long its last state still counts

struct api_state api;

struct conn {
    int fd;
    char *buf;
    size_t len, cap;
};

static struct conn watch = {.fd = -1};
static char cursor[64];  // the last applied frame's, to resume from
static double watch_retry, watch_heard, lost_at;
static char problem[160];  // the last thing logged about reaching the API, to log each only once

static struct command {
    struct conn c;
    int id;
    enum api_op op;
    char track[API_ID_LEN];
    double deadline;
} cmds[MAX_CMDS];

static void (*result_cb)(const struct api_result *);
static int last_id;
static char rid_prefix[48];
static struct json js;

static const char *const OPS[] = {"set_mute", "set_solo", "set_record_armed", "set_volume", "set_pan"};

static void trouble(const char *fmt, ...) {
    char msg[sizeof problem];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (strcmp(msg, problem)) say("API: %s", msg);
    snprintf(problem, sizeof problem, "%s", msg);
}

// --- connections ---

static int api_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", API_SOCKET);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {  // a Unix socket connects at once
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

// The request fits a fresh socket's buffer, so one send is enough. MSG_NOSIGNAL: no SIGPIPE.
static int send_line(int fd, const char *s, size_t len) {
    ssize_t n = send(fd, s, len, MSG_NOSIGNAL);
    if (n == (ssize_t)len) return 0;
    if (n >= 0) errno = EAGAIN;
    return -1;
}

// Reads one chunk: > 0 bytes added, 0 end of stream, -1 error (errno), -2 nothing more for now.
static ssize_t conn_fill(struct conn *c) {
    if (c->cap - c->len < 65537) {
        size_t cap = c->cap ? c->cap * 2 : 131072;
        if (cap > MAX_LINE) {
            errno = EMSGSIZE;
            return -1;
        }
        char *b = realloc(c->buf, cap);
        if (!b) return -1;
        c->buf = b;
        c->cap = cap;
    }
    ssize_t n = read(c->fd, c->buf + c->len, c->cap - c->len - 1);
    if (n > 0) {
        c->len += (size_t)n;
        return n;
    }
    if (n == 0) return 0;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -2 : -1;
}

// --- the model ---

// A string node made of [A-Za-z0-9_] that fits API_ID_LEN, or NULL: IDs go into requests as is.
static const char *id_of(int node) {
    const char *s = json_str(&js, node);
    if (!s || !*s || strlen(s) >= API_ID_LEN) return NULL;
    for (const char *p = s; *p; p++)
        if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') && *p != '_')
            return NULL;
    return s;
}

static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

static int fill_track(struct api_track *t, int node) {
    const char *id = id_of(json_path(&js, node, "ref.trackId"));
    if (!id) return -1;
    copy(t->id, sizeof t->id, id);
    copy(t->name, sizeof t->name, json_str(&js, json_get(&js, node, "name")));
    copy(t->type, sizeof t->type, json_str(&js, json_get(&js, node, "type")));
    t->mute = (signed char)json_bool(&js, json_path(&js, node, "requested.muted"));
    t->solo = (signed char)json_bool(&js, json_path(&js, node, "requested.soloed"));
    t->arm = (signed char)json_bool(&js, json_path(&js, node, "requested.recordArmed"));
    t->silenced = (signed char)json_bool(&js, json_get(&js, node, "silenced"));
    int mixer = json_get(&js, node, "mixer");
    copy(t->mixer_gen, sizeof t->mixer_gen, id_of(json_get(&js, mixer, "generation")));
    double v = json_num(&js, json_get(&js, mixer, "volume"), -1);
    t->volume = v >= 0 && v <= 1 ? (float)v : -1;
    v = json_num(&js, json_get(&js, mixer, "pan"), -1);
    t->pan = v >= 0 && v <= 1 ? (float)v : -1;
    return 0;
}

static void mute_context(struct api_state *s, int node) {
    const char *scope = json_str(&js, json_get(&js, node, "scope"));
    const char *seq = id_of(json_path(&js, node, "sequence.sequenceId"));
    const char *gen = id_of(json_path(&js, node, "sequence.generation"));
    s->mute_scope = 0;
    if (scope && !strcmp(scope, "global")) s->mute_scope = 'g';
    if (scope && !strcmp(scope, "sequence") && seq && gen) s->mute_scope = 's';
    copy(s->seq_id, sizeof s->seq_id, seq);
    copy(s->seq_gen, sizeof s->seq_gen, gen);
}

// A full state (the snapshot frame's "state"). Built aside: a bad one leaves the model as it was.
static int apply_snapshot(int st) {
    static struct api_state s;
    const char *inst = id_of(json_get(&js, st, "instanceId"));
    int tracks = json_get(&js, st, "tracks");
    if (!inst || tracks < 0) return -1;
    memset(&s, 0, sizeof s);
    copy(s.instance, sizeof s.instance, inst);
    copy(s.project, sizeof s.project, id_of(json_get(&js, st, "projectId")));
    for (int i = js.n[tracks].child; i >= 0; i = js.n[i].next) {
        if (s.n == API_MAX_TRACKS) return -1;
        if (json_num(&js, json_get(&js, i, "uiIndex"), -1) != s.n) return -1;  // Track List order
        if (fill_track(&s.t[s.n], i)) return -1;
        s.n++;
    }
    mute_context(&s, json_get(&js, st, "muteContext"));
    s.live = 1;
    int news = !api.live || strcmp(api.instance, s.instance) || strcmp(api.project, s.project) || api.n != s.n ||
               api.mute_scope != s.mute_scope;
    api = s;
    if (news)
        say("API: %s, project %s: %d tracks, mutes %s", api.instance, api.project[0] ? api.project : "none", api.n,
            api.mute_scope == 's' ? "per sequence" : api.mute_scope == 'g' ? "global" : "unknown");
    return 0;
}

// Controls for some tracks (a page of at most 16), matched by trackId. Names and order stay as the
// last snapshot had them: a topology change brings a new snapshot.
static int apply_change(int frame) {
    const char *inst = json_str(&js, json_get(&js, frame, "instanceId"));
    const char *proj = json_str(&js, json_get(&js, frame, "projectId"));
    int tracks = json_get(&js, frame, "tracks");
    if (!api.live || !inst || strcmp(inst, api.instance) || !proj || strcmp(proj, api.project) || tracks < 0)
        return -1;
    mute_context(&api, json_get(&js, frame, "muteContext"));
    for (int i = js.n[tracks].child; i >= 0; i = js.n[i].next) {
        struct api_track t;
        if (fill_track(&t, i)) continue;
        for (int k = 0; k < api.n; k++)
            if (!strcmp(api.t[k].id, t.id)) {
                api.t[k] = t;
                break;
            }
    }
    return 0;
}

// --- the watch ---

static void watch_close(void) {
    if (watch.fd >= 0) close(watch.fd);
    watch.fd = -1;
    watch.len = 0;
}

static void watch_lost(const char *why, double retry) {
    if (api.live && !lost_at) lost_at = now();
    trouble("watch lost: %s", why);
    watch_close();
    watch_retry = now() + retry;
}

static void keep_cursor(void) {
    const char *c = json_str(&js, json_get(&js, 0, "cursor"));
    if (c && strlen(c) < sizeof cursor && strspn(c, "run_0123456789abcdef:") == strlen(c)) strcpy(cursor, c);
    lost_at = 0;
}

static void watch_frame(char *s, size_t len) {
    watch_heard = now();
    if (json_parse(&js, s, len)) {
        cursor[0] = '\0';
        watch_lost("unreadable frame, starting over", 0);
        return;
    }
    const char *type = json_str(&js, json_get(&js, 0, "type"));
    const char *reason = json_str(&js, json_get(&js, 0, "reason"));
    if (!type) type = "?";
    if (verbose && strcmp(type, "heartbeat")) {
        const char *rev = json_str(&js, json_get(&js, 0, "revision"));
        say("API: %s %s, %zu bytes", type, rev ? rev : "", len);
    }
    if (!strcmp(type, "snapshot")) {
        if (apply_snapshot(json_get(&js, 0, "state")) == 0) keep_cursor();
        else say("API: unusable snapshot, keeping the last state");
    } else if (!strcmp(type, "change")) {
        if (apply_change(0) == 0) keep_cursor();
    } else if (!strcmp(type, "ack")) {
        problem[0] = '\0';
        if (json_bool(&js, json_get(&js, 0, "resumed")) == 1) lost_at = 0;  // the state carries on
    } else if (!strcmp(type, "resync_required")) {
        cursor[0] = '\0';
        watch_lost(reason ? reason : "resync required", 0);
    } else if (!strcmp(type, "terminated")) {
        watch_lost(reason ? reason : "terminated", RETRY_S);
    } else if (strcmp(type, "heartbeat")) {
        say("API: unknown watch frame \"%s\"", type);
    }
}

static void watch_open(double t) {
    char req[128];
    int len = cursor[0] ? snprintf(req, sizeof req, "{\"apiVersion\":\"0.1\",\"op\":\"watch\",\"cursor\":\"%s\"}\n", cursor)
                        : snprintf(req, sizeof req, "{\"apiVersion\":\"0.1\",\"op\":\"watch\"}\n");
    int fd = api_connect();
    if (fd < 0 || send_line(fd, req, (size_t)len) < 0) {
        trouble("can't reach %s: %s", API_SOCKET, strerror(errno));
        if (fd >= 0) close(fd);
        watch_retry = t + RETRY_S;
        return;
    }
    watch.fd = fd;
    watch.len = 0;
    watch_heard = t;
    if (verbose) say("API: watching%s", cursor[0] ? " from the last cursor" : "");
}

static void watch_pump(void) {
    for (;;) {
        ssize_t r = conn_fill(&watch);
        if (r == -2) return;
        if (r <= 0) {
            watch_lost(r == 0 ? "closed by the API" : strerror(errno), 0.2);
            return;
        }
        size_t start = 0;
        char *nl;
        while (watch.fd >= 0 && (nl = memchr(watch.buf + start, '\n', watch.len - start))) {
            *nl = '\0';
            watch_frame(watch.buf + start, (size_t)(nl - (watch.buf + start)));
            start = (size_t)(nl + 1 - watch.buf);
        }
        if (watch.fd < 0) return;  // a frame ended the watch
        memmove(watch.buf, watch.buf + start, watch.len - start);
        watch.len -= start;
    }
}

// --- commands ---

static void command_done(struct command *c, char *line, size_t len, const char *why) {
    struct api_result r = {.id = c->id, .op = c->op, .status = "failed", .error = why, .total_ms = -1};
    copy(r.track, sizeof r.track, c->track);
    if (line && json_parse(&js, line, len)) {
        r.error = "unreadable receipt";
    } else if (line) {
        // A request refused before it was accepted has an error but no receipt status.
        const char *status = json_str(&js, json_get(&js, 0, "status"));
        if (status) r.status = status;
        r.error = json_str(&js, json_path(&js, 0, "error.code"));
        r.total_ms = (int)json_num(&js, json_path(&js, 0, "timing.totalMs"), -1);
    }
    result_cb(&r);  // before the slot is freed: r points into its buffer
    close(c->c.fd);
    c->c.fd = -1;
    c->c.len = 0;
}

static void command_pump(struct command *c) {
    for (;;) {
        size_t old = c->c.len;
        ssize_t r = conn_fill(&c->c);
        if (r == -2) return;
        if (r <= 0) {
            command_done(c, NULL, 0, r == 0 ? "closed without a receipt" : strerror(errno));
            return;
        }
        char *nl = memchr(c->c.buf + old, '\n', c->c.len - old);
        if (nl) {
            *nl = '\0';
            command_done(c, c->c.buf, (size_t)(nl - c->c.buf), NULL);
            return;
        }
    }
}

int api_send(enum api_op op, int t, double value, int expected) {
    struct command *c = NULL;
    for (int i = 0; i < MAX_CMDS && !c; i++)
        if (cmds[i].c.fd < 0) c = &cmds[i];
    if (!api.live || t < 0 || t >= api.n || !api.project[0]) return -1;
    const struct api_track *tr = &api.t[t];
    if (!c) {
        say("API: %d commands already waiting, not sending more", MAX_CMDS);
        return -1;
    }
    int mixer = op == API_VOLUME || op == API_PAN;
    if ((op == API_MUTE && !api.mute_scope) || (mixer && !tr->mixer_gen[0])) return -1;

    char req[1024];
    int id = ++last_id;
    size_t len = (size_t)snprintf(req, sizeof req,
                                  "{\"apiVersion\":\"0.1\",\"op\":\"%s\",\"requestId\":\"%s-%d\",\"target\":{\"instanceId\":"
                                  "\"%s\",\"projectId\":\"%s\",\"trackId\":\"%s\"}",
                                  OPS[op], rid_prefix, id, api.instance, api.project, tr->id);
    if (mixer) {
        value = value < 0 ? 0 : value > 1 ? 1 : value;
        len += (size_t)snprintf(req + len, sizeof req - len, ",\"mixerGeneration\":\"%s\",\"value\":%.9g", tr->mixer_gen,
                                value);
    } else {
        len += (size_t)snprintf(req + len, sizeof req - len, ",\"value\":%s", value ? "true" : "false");
        if (expected >= 0)
            len += (size_t)snprintf(req + len, sizeof req - len, ",\"expected\":{\"value\":%s}", expected ? "true" : "false");
        if (op == API_MUTE && api.mute_scope == 's')
            len += (size_t)snprintf(req + len, sizeof req - len,
                                    ",\"muteScope\":\"sequence\",\"sequence\":{\"instanceId\":\"%s\",\"projectId\":\"%s\","
                                    "\"sequenceId\":\"%s\",\"generation\":\"%s\"}",
                                    api.instance, api.project, api.seq_id, api.seq_gen);
    }
    len += (size_t)snprintf(req + len, sizeof req - len, ",\"waitMs\":%d}\n", WAIT_MS);
    if (len >= sizeof req) return -1;

    int fd = api_connect();
    if (fd < 0 || send_line(fd, req, len) < 0) {
        say("API: can't send %s: %s", OPS[op], strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    if (verbose) say("API: > %.*s", (int)len - 1, req);
    c->c.fd = fd;
    c->c.len = 0;
    c->id = id;
    c->op = op;
    copy(c->track, sizeof c->track, tr->id);
    c->deadline = now() + CMD_LIMIT;
    return id;
}

// --- the poll loop's side ---

void api_init(void (*on_result)(const struct api_result *)) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    // Request IDs stay unique across restarts: the API keeps receipts for 10 minutes per caller.
    snprintf(rid_prefix, sizeof rid_prefix, "midimixd-%d-%lld", (int)getpid(), (long long)ts.tv_sec);
    result_cb = on_result;
    for (int i = 0; i < MAX_CMDS; i++) cmds[i].c.fd = -1;
}

int api_pollfds(struct pollfd *fds, int max) {
    int n = 0;
    if (watch.fd >= 0 && n < max) fds[n++] = (struct pollfd){.fd = watch.fd, .events = POLLIN};
    for (int i = 0; i < MAX_CMDS && n < max; i++)
        if (cmds[i].c.fd >= 0) fds[n++] = (struct pollfd){.fd = cmds[i].c.fd, .events = POLLIN};
    return n;
}

void api_events(const struct pollfd *fds, int n) {
    for (int k = 0; k < n; k++) {
        if (!fds[k].revents) continue;
        if (fds[k].fd == watch.fd) {
            watch_pump();
            continue;
        }
        for (int i = 0; i < MAX_CMDS; i++)
            if (cmds[i].c.fd == fds[k].fd) command_pump(&cmds[i]);
    }
}

void api_tick(double t) {
    if (watch.fd < 0 && t >= watch_retry) watch_open(t);
    if (watch.fd >= 0 && t - watch_heard > SILENT_S) watch_lost("silent", 0);
    if (api.live && lost_at && t - lost_at > LOST_S) {
        api.live = 0;
        lost_at = 0;
        say("API: no current state");
    }
    for (int i = 0; i < MAX_CMDS; i++)
        if (cmds[i].c.fd >= 0 && t > cmds[i].deadline) command_done(&cmds[i], NULL, 0, "no receipt in time");
}

void api_close(void) {
    watch_close();  // closing the connection unsubscribes
    for (int i = 0; i < MAX_CMDS; i++)
        if (cmds[i].c.fd >= 0) {
            close(cmds[i].c.fd);
            cmds[i].c.fd = -1;
        }
}
