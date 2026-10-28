// mpcstate: print every track's mute / solo / rec arm / volume, read from the running MPC app's
// memory (mpcread: read-only, MPC 3.9.1.2 only). A check independent of the native API, which is
// what midimixd sees.
//
//   mpcstate show                          the selected sequence and track, every user track
//   mpcstate watch [secs] [hz]             poll (30 Hz by default), print what changes, then timings
//   mpcstate dump project|track|program [index] <bytes>   raw object bytes on stdout, to diff
//   mpcstate dump at <hex address> <bytes>

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "../mpcread/mpcread.h"

static void die(const char *what) {
    fprintf(stderr, "mpcstate: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void fail(const char *what) {
    fprintf(stderr, "mpcstate: %s\n", what);
    exit(1);
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// mpc_attach, printing what it found (always when it fails, else unless quiet).
static int attach(int quiet) {
    struct mpc_found f;
    const char *why = mpc_attach(&f);
    if (f.where && (!quiet || why))
        printf("MPC pid %d, load address 0x%x: %d Project in %s, %.0f MB scanned in %.0f ms\n", f.pid,
               f.base, f.projects, f.where, f.mb, f.ms);
    if (why) {
        if (!quiet) fprintf(stderr, "mpcstate: %s\n", why);
        return -1;
    }
    if (!quiet) printf("Project at 0x%x\n", f.project);
    return 0;
}

// --- output ---

static void print_selected(const struct mpc_state *s) {
    if (s->sel_index < 0)
        printf("selected track: none of the user tracks (0x%x), index field %u\n", s->sel_track,
               s->sel_index_field);
    else if ((uint32_t)s->sel_index != s->sel_index_field)
        printf("selected track %d, index field %u\n", s->sel_index + 1, s->sel_index_field);
    else
        printf("selected track %d\n", s->sel_index + 1);
}

static void print_tracks(const struct mpc_state *s) {
    printf("sequence %d \"%s\" %.2f bpm, %d tracks, ", s->seq_index + 1, s->seq_name, s->bpm, s->n);
    print_selected(s);
    printf("     #  type          mute solo arm silenced  volume    name\n");
    for (int i = 0; i < s->n; i++) {
        const struct mpc_track *t = &s->t[i];
        printf("  %c %2d  %-13s %-4s %-4s %-3s %-8s  %+6.2f dB %s\n", i == s->sel_index ? '>' : ' ', i + 1,
               mpc_program_type(t->program_vt), t->mute ? "M" : ".", t->solo ? "S" : ".", t->arm ? "A" : ".",
               t->silenced ? "x" : ".", 20 * log10f(t->volume), t->name);
    }
}

static int same_tracks(const struct mpc_state *a, const struct mpc_state *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++)
        if (a->t[i].addr != b->t[i].addr || a->t[i].program != b->t[i].program) return 0;
    return 1;
}

static void print_changes(const struct mpc_state *a, const struct mpc_state *b, double t) {
    if (!same_tracks(a, b)) {
        printf("%9.3f tracks changed:\n", t);
        print_tracks(b);
        return;
    }
    if (a->sel_seq != b->sel_seq)
        printf("%9.3f sequence %d \"%s\" %.2f bpm\n", t, b->seq_index + 1, b->seq_name, b->bpm);
    else if (a->bpm != b->bpm)
        printf("%9.3f bpm %.2f\n", t, b->bpm);
    if (a->sel_track != b->sel_track || a->sel_index_field != b->sel_index_field) {
        printf("%9.3f ", t);
        print_selected(b);
    }
    for (int i = 0; i < b->n; i++) {
        const struct mpc_track *x = &a->t[i], *y = &b->t[i];
        static const char *const what[] = {"mute", "solo", "arm", "silenced"};
        const unsigned char was[] = {x->mute, x->solo, x->arm, x->silenced};
        const unsigned char is[] = {y->mute, y->solo, y->arm, y->silenced};
        for (int k = 0; k < 4; k++)
            if (was[k] != is[k]) printf("%9.3f track %2d %-8s %d  (%s)\n", t, i + 1, what[k], is[k], y->name);
        if (x->volume != y->volume)
            printf("%9.3f track %2d volume   %+.2f dB  (%s)\n", t, i + 1, 20 * log10f(y->volume), y->name);
        if (strcmp(x->name, y->name)) printf("%9.3f track %2d renamed \"%s\"\n", t, i + 1, y->name);
    }
}

// --- commands ---

static int cmd_show(void) {
    static struct mpc_state s;
    if (attach(0)) return 1;
    double t0 = now();
    const char *why = mpc_poll(&s);
    if (why) fail(why);
    printf("read in %.0f us\n", (now() - t0) * 1e6);
    print_tracks(&s);
    return 0;
}

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int cmd_watch(double secs, double hz) {
    static struct mpc_state a, b;
    static double times[1 << 20];
    struct mpc_state *prev = &a, *cur = &b;
    long polls = 0, bad = 0, attaches = 0, ticks = 0;
    if (attach(0)) return 1;
    const char *why = mpc_poll(prev);
    if (why) fail(why);
    print_tracks(prev);
    fflush(stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct rusage ru0;
    getrusage(RUSAGE_SELF, &ru0);
    double start = now();
    long period = (long)(1e9 / hz);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int failing = 0, lost = 0;
    const char *last_why = NULL;
    while (!stop && (secs <= 0 || now() - start < secs)) {
        next.tv_nsec += period;
        while (next.tv_nsec >= 1000000000) next.tv_nsec -= 1000000000, next.tv_sec++;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        if (lost) {  // MPC restarting or loading: search again every 2 s
            if (++ticks % (long)(2 * hz)) continue;
            if (attach(1) || mpc_poll(prev)) continue;
            lost = 0;
            last_why = NULL;
            attaches++;
            printf("%9.3f found MPC again (pid %d, Project 0x%x)\n", now() - start, mpc_pid, mpc_project);
            print_tracks(prev);
            continue;
        }
        double t0 = now();
        why = mpc_poll(cur);
        double dt = now() - t0;
        if (polls < (long)(sizeof times / sizeof *times)) times[polls] = dt;
        polls++;
        if (why) {
            bad++;
            if (why != last_why) printf("%9.3f bad poll: %s\n", t0 - start, why);
            last_why = why;
            // Not a torn read and the Project is gone: look for it again. While it is still there,
            // keep polling it (MPC may be in the middle of a change).
            if (++failing >= 3 && !mpc_project_alive()) {
                printf("%9.3f lost the Project (%s), searching again\n", t0 - start, why);
                double t1 = now();
                if (attach(1) == 0 && mpc_poll(prev) == NULL) {
                    attaches++;
                    last_why = NULL;
                    printf("%9.3f Project 0x%x (search took %.0f ms)\n", now() - start, mpc_project,
                           (now() - t1) * 1000);
                    print_tracks(prev);
                } else {
                    lost = 1;
                }
                failing = 0;
            }
            continue;
        }
        if (failing >= 3) printf("%9.3f good again after %d bad polls\n", t0 - start, failing);
        failing = 0;
        last_why = NULL;
        print_changes(prev, cur, t0 - start);
        struct mpc_state *x = prev;
        prev = cur;
        cur = x;
    }

    double elapsed = now() - start;
    struct rusage ru1;
    getrusage(RUSAGE_SELF, &ru1);
    double cpu = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e6 +
                 (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;
    long n = polls < (long)(sizeof times / sizeof *times) ? polls : (long)(sizeof times / sizeof *times);
    double sum = 0;
    for (long i = 0; i < n; i++) sum += times[i];
    qsort(times, (size_t)n, sizeof *times, cmp_double);
    printf("%ld polls in %.1f s, %ld bad, %ld new searches; poll %.0f us mean, %.0f median, %.0f p99, "
           "%.0f max; CPU %.2f%% of a core\n",
           polls, elapsed, bad, attaches, n ? sum / n * 1e6 : 0, n ? times[n / 2] * 1e6 : 0,
           n ? times[n * 99 / 100] * 1e6 : 0, n ? times[n - 1] * 1e6 : 0, cpu / elapsed * 100);
    return 0;
}

static int cmd_dump(const char *what, uint32_t arg, size_t len) {
    static struct mpc_state s;
    static unsigned char buf[1 << 16];
    if (len == 0 || len > sizeof buf) fail("dump: 1 to 65536 bytes");
    uint32_t addr = arg;
    if (!strcmp(what, "at")) {  // a raw address: no need for the Project
        if ((mpc_pid = mpc_find_pid()) <= 0) fail("MPC is not running, or more than one is");
    } else {
        if (attach(1)) return 1;
        const char *why = mpc_poll(&s);
        if (why) fail(why);
        if (!strcmp(what, "project")) {
            addr = mpc_project;
        } else {
            if (arg < 1 || (int)arg > s.n) fail("dump: no such track");
            addr = !strcmp(what, "track") ? s.t[arg - 1].addr : s.t[arg - 1].program;
        }
        fprintf(stderr, "%s at 0x%x, %zu bytes\n", what, addr, len);
    }
    if (mpc_read(addr, buf, len)) die("read");
    fwrite(buf, 1, len, stdout);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "show")) return cmd_show();
    if (argc >= 2 && !strcmp(argv[1], "watch"))
        return cmd_watch(argc > 2 ? atof(argv[2]) : 0, argc > 3 ? atof(argv[3]) : 30);
    if (argc == 4 && !strcmp(argv[1], "dump") && !strcmp(argv[2], "project"))
        return cmd_dump(argv[2], 0, strtoul(argv[3], NULL, 0));
    if (argc == 5 && !strcmp(argv[1], "dump") && (!strcmp(argv[2], "track") || !strcmp(argv[2], "program")))
        return cmd_dump(argv[2], (uint32_t)strtoul(argv[3], NULL, 10), strtoul(argv[4], NULL, 0));
    if (argc == 5 && !strcmp(argv[1], "dump") && !strcmp(argv[2], "at"))
        return cmd_dump(argv[2], (uint32_t)strtoul(argv[3], NULL, 16), strtoul(argv[4], NULL, 0));
    fprintf(stderr, "usage: mpcstate show | watch [secs] [hz] | dump project <bytes> | "
                    "dump track|program <n> <bytes> | dump at <hex address> <bytes>\n");
    return 2;
}
