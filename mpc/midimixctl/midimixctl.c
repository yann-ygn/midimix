// midimixctl: talks to midimixd's control socket (midimixd/ctl.h), for tests and by hand.
//
//   midimixctl get                 prints midimixd's state once
//   midimixctl watch               prints it whenever it changes, until stopped
//   midimixctl map <row=target>... sets a mapping (midimixd/map.h), then watches: the mapping lasts
//                                  as long as this connection, so until midimixctl is stopped
//
// Exits with status 1 when midimixd can't be reached or refuses the request.

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../midimixd/ctl.h"

static int fd;
static char buf[8192];
static size_t len;

// Sends a request and reads its reply (up to the empty line) into reply. 0, or -1.
static int ask(const char *req, char *reply, size_t cap) {
    size_t n = strlen(req);
    if (write(fd, req, n) != (ssize_t)n) return -1;
    for (;;) {
        char *end = memmem(buf, len, "\n\n", 2);
        if (end) {
            size_t m = (size_t)(end + 1 - buf);
            if (m >= cap) return -1;
            memcpy(reply, buf, m);
            reply[m] = '\0';
            len -= m + 1;
            memmove(buf, end + 2, len);
            return 0;
        }
        if (len == sizeof buf) return -1;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 2000) <= 0) return -1;
        ssize_t r = read(fd, buf + len, sizeof buf - len);
        if (r <= 0) return -1;
        len += (size_t)r;
    }
}

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "get") && strcmp(argv[1], "watch") && strcmp(argv[1], "map"))) {
        fprintf(stderr, "usage: midimixctl get | watch | map <row=target>...\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CTL_SOCKET);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "midimixctl: %s: %s\n", CTL_SOCKET, strerror(errno));
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    char reply[4096], last[4096] = "", req[1024];
    if (!strcmp(argv[1], "map")) {
        size_t n = (size_t)snprintf(req, sizeof req, "map");
        for (int i = 2; i < argc && n < sizeof req; i++) n += (size_t)snprintf(req + n, sizeof req - n, " %s", argv[i]);
        if (n + 2 > sizeof req) return 2;
        strcat(req, "\n");
        if (ask(req, reply, sizeof reply) || strncmp(reply, "ok\n", 3)) {
            fprintf(stderr, "midimixctl: %s", reply[0] ? reply : "no reply\n");
            return 1;
        }
    }
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    do {
        if (ask("get\n", reply, sizeof reply)) {
            if (!stop) fprintf(stderr, "midimixctl: no reply from midimixd\n");
            return stop ? 0 : 1;
        }
        if (strcmp(reply, last)) {
            fputs(reply, stdout);
            if (strcmp(argv[1], "get")) putchar('\n');
            strcpy(last, reply);
        }
        if (!strcmp(argv[1], "get")) break;
        usleep(100000);
    } while (!stop);
    return 0;
}
