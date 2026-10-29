/* mmd: see mmd.h. The protocol is ../midimixd/ctl.h's: a request per line, a reply ending with an empty line. */
#define _GNU_SOURCE
#include "mmd.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define POLL_MS  100    /* between two gets */
#define RETRY_MS 1000   /* between two attempts to reach midimixd */
#define REPLY_MS 1000   /* how long a reply may take */

struct mmd {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int stop;                     /* under lock, as the four below */
    unsigned char map[MAP_ROWS];
    unsigned map_seq;             /* +1 on every mmd_set_map */
    mmd_state_t state;
    unsigned rev;
    char path[108];               /* the thread's own from here */
    int fd;
    char buf[8192];
    size_t len;
};

static int connect_to(const char *path)
{
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

/* Sends req and reads its reply, up to the empty line, into out. 0, or -1: the connection is no use any more. */
static int ask(mmd_t *m, const char *req, char *out, size_t cap)
{
    size_t n = strlen(req);
    if (send(m->fd, req, n, MSG_NOSIGNAL) != (ssize_t)n)
        return -1;
    for (;;) {
        char *end = memmem(m->buf, m->len, "\n\n", 2);
        if (end) {
            size_t k = (size_t)(end + 1 - m->buf);
            if (k >= cap)
                return -1;
            memcpy(out, m->buf, k);
            out[k] = 0;
            m->len -= k + 1;
            memmove(m->buf, end + 2, m->len);
            return 0;
        }
        struct pollfd p = {.fd = m->fd, .events = POLLIN};
        if (m->len == sizeof m->buf || poll(&p, 1, REPLY_MS) <= 0)
            return -1;
        ssize_t r = read(m->fd, m->buf + m->len, sizeof m->buf - m->len);
        if (r <= 0)
            return -1;
        m->len += (size_t)r;
    }
}

/* A get's reply (midimixd.c's ctl_state) into s, which starts zeroed. */
static void parse(const char *text, mmd_state_t *s)
{
    for (const char *line = text; *line;) {
        const char *nl = strchr(line, '\n');
        char l[256], a[32], b[32];
        int n, k;
        unsigned u;
        snprintf(l, sizeof l, "%.*s", (int)(nl ? nl - line : (long)strlen(line)), line);
        line = nl ? nl + 1 : line + strlen(line);
        if (sscanf(l, "midimix %d", &s->midimix) == 1 || sscanf(l, "api %d", &s->api) == 1
            || sscanf(l, "bank %d %d", &s->bank, &s->banks) == 2 || sscanf(l, "tracks %d", &s->tracks) == 1
            || sscanf(l, "mapfrom %7s", s->mapfrom) == 1 || sscanf(l, "selected %d", &s->selected) == 1)
            continue;
        if (sscanf(l, "touch %u %31s %d", &u, a, &k) == 3) {
            s->touch_seq = u;
            for (int r = 0; r < MAP_ROWS; r++)
                if (!strcmp(a, map_row_name[r]))
                    s->touch_row = r;
        } else if (sscanf(l, "strip %d %31s %31s %n", &k, a, b, &n) == 3 && k >= 1 && k <= MMD_STRIPS) {
            if (!strcmp(b, "-"))
                continue;   /* no track */
            s->strip[k - 1].present = 1;
            s->strip[k - 1].colour = strcmp(a, "-") ? strtol(a, NULL, 16) : -1;
            snprintf(s->strip[k - 1].type, sizeof s->strip[k - 1].type, "%.15s", b);
            snprintf(s->strip[k - 1].name, sizeof s->strip[k - 1].name, "%s", l + n);
        }
    }
}

static void *run(void *arg)
{
    mmd_t *m = arg;
    unsigned sent = 0;    /* the map_seq midimixd has */
    int need_map = 1;     /* a new connection: send the mapping whatever its seq */
    char error[96] = "", reply[4096], req[MAP_SPEC_MAX + 8];
    pthread_mutex_lock(&m->lock);
    while (!m->stop) {
        unsigned seq = m->map_seq;
        unsigned char map[MAP_ROWS];
        memcpy(map, m->map, sizeof map);
        pthread_mutex_unlock(&m->lock);

        mmd_state_t s;
        memset(&s, 0, sizeof s);   /* padding too: states are compared with memcmp */
        if (m->fd < 0 && (m->fd = connect_to(m->path)) >= 0) {
            m->len = 0;
            need_map = 1;
        }
        if (m->fd >= 0) {
            int ok = 1;
            if (need_map || seq != sent) {
                strcpy(req, "map ");
                map_format(map, req + 4, sizeof req - 5);
                strcat(req, "\n");
                ok = !ask(m, req, reply, sizeof reply);
                if (ok) {
                    snprintf(error, sizeof error, "%.95s", strncmp(reply, "error ", 6) ? "" : reply + 6);
                    error[strcspn(error, "\n")] = 0;
                    sent = seq;
                    need_map = 0;
                }
            }
            if (ok && (ok = !ask(m, "get\n", reply, sizeof reply))) {
                parse(reply, &s);
                s.up = 1;
                snprintf(s.error, sizeof s.error, "%s", error);
            }
            if (!ok) {
                close(m->fd);
                m->fd = -1;
            }
        }

        pthread_mutex_lock(&m->lock);
        if (memcmp(&s, &m->state, sizeof s)) {
            m->state = s;
            m->rev++;
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        long ms = m->fd >= 0 ? POLL_MS : RETRY_MS;
        until.tv_sec += ms / 1000;
        until.tv_nsec += (ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        while (!m->stop && m->map_seq == seq && pthread_cond_timedwait(&m->wake, &m->lock, &until) != ETIMEDOUT)
            ;
    }
    pthread_mutex_unlock(&m->lock);
    if (m->fd >= 0)
        close(m->fd);
    return NULL;
}

mmd_t *mmd_start(const char *path, const unsigned char map[MAP_ROWS])
{
    mmd_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    snprintf(m->path, sizeof m->path, "%s", path);
    memcpy(m->map, map, sizeof m->map);
    m->fd = -1;
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->wake, NULL);
    if (pthread_create(&m->thread, NULL, run, m)) {
        free(m);
        return NULL;
    }
    return m;
}

void mmd_stop(mmd_t *m)
{
    pthread_mutex_lock(&m->lock);
    m->stop = 1;
    pthread_cond_signal(&m->wake);
    pthread_mutex_unlock(&m->lock);
    pthread_join(m->thread, NULL);
    pthread_cond_destroy(&m->wake);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

void mmd_set_map(mmd_t *m, const unsigned char map[MAP_ROWS])
{
    pthread_mutex_lock(&m->lock);
    if (memcmp(m->map, map, sizeof m->map)) {
        memcpy(m->map, map, sizeof m->map);
        m->map_seq++;
        pthread_cond_signal(&m->wake);
    }
    pthread_mutex_unlock(&m->lock);
}

unsigned mmd_state(mmd_t *m, mmd_state_t *out)
{
    pthread_mutex_lock(&m->lock);
    *out = m->state;
    unsigned rev = m->rev;
    pthread_mutex_unlock(&m->lock);
    return rev;
}
