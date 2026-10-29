// ctl: see ctl.h. Replies are small (a get is about 1 KB) and go out with one non-blocking send; a
// client that doesn't read them is dropped rather than waited on.

#define _GNU_SOURCE
#include "ctl.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "api.h"  // say
#include "map.h"

#define CLIENTS  (CTL_MAX_FDS - 1)
#define LINE_MAX 512

static int listen_fd = -1;

static struct client {
    int fd;  // -1 for a free slot
    char in[LINE_MAX];
    size_t len;
    unsigned char map[MAP_ROWS];
    unsigned map_seq;  // when its mapping was set (0: none)
} clients[CLIENTS];

static unsigned map_seq;

int ctl_init(void) {
    for (int i = 0; i < CLIENTS; i++) clients[i].fd = -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        say("control socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CTL_SOCKET);
    unlink(CTL_SOCKET);  // a dead midimixd's: we hold the lock, so it's no one's
    mode_t old = umask(077);
    int r = bind(fd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r < 0 || listen(fd, 4) < 0) {
        say("control socket %s: %s", CTL_SOCKET, strerror(errno));
        close(fd);
        return -1;
    }
    listen_fd = fd;
    return 0;
}

static void drop(struct client *c, const char *why) {
    if (why) say("control: client %d dropped: %s", (int)(c - clients), why);
    else if (verbose) say("control: client %d gone", (int)(c - clients));
    close(c->fd);
    c->fd = -1;
}

static void reply(struct client *c, const char *s, size_t len) {
    ssize_t n = send(c->fd, s, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n != (ssize_t)len) drop(c, n < 0 ? strerror(errno) : "not reading its replies");
}

static void request(struct client *c, char *line) {
    char out[4096], err[120];
    size_t n;
    if (verbose) say("control %d: %s", (int)(c - clients), line);
    if (!strcmp(line, "get")) {
        n = ctl_state((int)(c - clients), out, sizeof out);
    } else if (!strncmp(line, "map ", 4) || !strcmp(line, "map")) {
        unsigned char m[MAP_ROWS];
        memcpy(m, map_default, sizeof m);
        if (map_parse(m, line + 3, err, sizeof err)) {
            n = (size_t)snprintf(out, sizeof out, "error %s\n\n", err);
        } else {
            memcpy(c->map, m, sizeof m);
            c->map_seq = ++map_seq;
            n = (size_t)snprintf(out, sizeof out, "ok\n\n");
        }
    } else if (!strcmp(line, "unmap")) {
        c->map_seq = 0;
        n = (size_t)snprintf(out, sizeof out, "ok\n\n");
    } else {
        n = (size_t)snprintf(out, sizeof out, "error unknown request\n\n");
    }
    reply(c, out, n < sizeof out ? n : sizeof out - 1);
}

static void pump(struct client *c) {
    for (;;) {
        ssize_t r = read(c->fd, c->in + c->len, sizeof c->in - c->len);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
            drop(c, r < 0 ? strerror(errno) : NULL);
            return;
        }
        if (r < 0) return;
        c->len += (size_t)r;
        char *nl;
        while (c->fd >= 0 && (nl = memchr(c->in, '\n', c->len))) {
            *nl = '\0';
            if (nl > c->in && nl[-1] == '\r') nl[-1] = '\0';
            request(c, c->in);
            if (c->fd < 0) return;
            c->len -= (size_t)(nl + 1 - c->in);
            memmove(c->in, nl + 1, c->len);
        }
        if (c->len == sizeof c->in) {
            drop(c, "request too long");
            return;
        }
    }
}

static void accept_all(void) {
    int fd;
    while ((fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
        struct client *c = NULL;
        for (int i = 0; i < CLIENTS && !c; i++)
            if (clients[i].fd < 0) c = &clients[i];
        if (!c) {
            say("control: %d clients already, refusing one", CLIENTS);
            close(fd);
            continue;
        }
        *c = (struct client){.fd = fd};
        if (verbose) say("control: client %d", (int)(c - clients));
    }
}

int ctl_pollfds(struct pollfd *fds, int max) {
    int n = 0;
    if (listen_fd >= 0 && n < max) fds[n++] = (struct pollfd){.fd = listen_fd, .events = POLLIN};
    for (int i = 0; i < CLIENTS && n < max; i++)
        if (clients[i].fd >= 0) fds[n++] = (struct pollfd){.fd = clients[i].fd, .events = POLLIN};
    return n;
}

void ctl_events(const struct pollfd *fds, int n) {
    for (int k = 0; k < n; k++) {
        if (!fds[k].revents) continue;
        if (fds[k].fd == listen_fd) {
            accept_all();
            continue;
        }
        for (int i = 0; i < CLIENTS; i++)
            if (clients[i].fd >= 0 && clients[i].fd == fds[k].fd) pump(&clients[i]);
    }
}

const unsigned char *ctl_map(int *owner) {
    const struct client *best = NULL;
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].map_seq && (!best || clients[i].map_seq > best->map_seq)) best = &clients[i];
    *owner = best ? (int)(best - clients) : -1;
    return best ? best->map : map_default;
}

void ctl_close(void) {
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].fd >= 0) drop(&clients[i], NULL);
    if (listen_fd >= 0) {
        close(listen_fd);
        unlink(CTL_SOCKET);
        listen_fd = -1;
    }
}
