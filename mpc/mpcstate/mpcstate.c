// mpcstate: read every track's mute / solo / rec arm straight from the running MPC app's memory.
// Read-only: process_vm_readv as root, no ptrace stop, no writes, no calls into MPC. Prototype of
// midimixd's reader. Method and offsets come from mpc-tests (../mpc/findings/live-state.md) and
// only fit MPC 3.9.1.2: the executable's GNU build ID is checked before any offset is used.
//
//   mpcstate show                          the selected sequence and track, every user track
//   mpcstate watch [secs] [hz]             poll (30 Hz by default), print what changes, then timings
//   mpcstate dump project|track|program [index] <bytes>   raw object bytes on stdout, to diff
//   mpcstate dump at <hex address> <bytes>
//
// Each poll is 5 process_vm_readv calls, one per level of pointers: Project, TrackPool and the
// current Sequence, the track vector, the Tracks, then their Programs and names.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

// MPC 3.9.1.2 (Key 37), from ../mpc/tools/profiles/key37-mpc3.9.1.2.json. Virtual tables are
// offsets from the executable's load address. The TrackPool holds several vectors of Track
// pointers (begin, end, capacity): +0x34 every track, buses included; +0x44 the non-audio user
// tracks (mpc-tests' "normal" tracks); +0x54 audio tracks and buses; +0x64 plugin, keygroup, MIDI
// and CV; +0x74 every user track in the UI's order (this one); +0x84 buses; then one per type
// (+0x94 audio, +0xb4 CV, +0xc4 drum, +0xd4 keygroup, +0xe4 MIDI, +0xf4 plugin) and +0x104 outputs,
// +0x114 returns, +0x124 submixes. Found 2026-10-24 with 10 tracks of every type.
static const unsigned char build_id[20] = {
    0x83, 0x71, 0x19, 0x9f, 0x94, 0x5e, 0xcc, 0x40, 0xf7, 0x9c,
    0xb1, 0x9c, 0x17, 0xff, 0x5d, 0xe7, 0x28, 0x10, 0xa9, 0x1c,
};
#define BUILD_ID_FILE_OFFSET 524

#define VT_PROJECT   0x6933c4cu
#define VT_TRACKPOOL 0x68ad2c4u
#define VT_TRACK     0x6935554u
#define VT_SEQUENCE  0x6934b18u

static const struct { uint32_t off, vt; } project_secondary[] = {
    {0x14, 0x6933ce0}, {0x1c, 0x6933d44}, {0x24, 0x6933d74}, {0x454, 0x6933ddc}, {0x45c, 0x6933e0c},
};

static const struct { uint32_t vt; const char *name; } program_types[] = {
    {0x692fec4, "audio"},  {0x69302ec, "clip"},          {0x6930a14, "cv"},     {0x6930c00, "drum"},
    {0x693147c, "keygroup"}, {0x69319d0, "input"},       {0x6931bb0, "output"},
    {0x6931d90, "master-output"}, {0x6931f70, "return"}, {0x6932150, "submix"},
    {0x6932338, "midi"},   {0x6932a48, "plugin"},
};

#define PROJECT_SIZE        0xaa8
#define P_SEL_TRACK         0xa0
#define P_SEL_TRACK_MIRROR  0xb0
#define P_SEL_SEQ           0xe4
#define P_SEL_SEQ_MIRROR    0xf4
#define P_SEL_TRACK_INDEX   0x15c
#define P_SEQ_ARRAY         0xa70
#define P_SEQ_CAPACITY      0xa74
#define P_SEQ_COUNT         0xa78
#define P_TRACKPOOL         0xa94
#define POOL_SIZE           0x80
#define POOL_TRACKS_BEGIN   0x74
#define POOL_TRACKS_END     0x78
#define POOL_TRACKS_CAP     0x7c
#define T_NAME              0x468
#define T_REC_ARM           0x494
#define T_PROGRAM           0x648
#define S_NAME              0x6c
#define S_BPM               0xc4
#define PR_MUTE             0x78
#define PR_SOLO             0xc0
#define PR_SILENCED         0x1ac

#define MAX_TRACKS 256
#define MAX_SEQS   2048
#define NAME_LEN   48

struct track {
    uint32_t addr, program, name_ptr, program_vt;
    unsigned char arm, mute, solo, silenced;
    char name[NAME_LEN];
};

struct state {
    uint32_t sel_track, sel_seq, sel_index_field;
    int sel_index, seq_index, n;
    float bpm;
    char seq_name[NAME_LEN];
    struct track t[MAX_TRACKS];
};

static pid_t pid;
static uint32_t base, project;

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

static uint32_t u32(const unsigned char *b, size_t off) {
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}

static const char *program_type(uint32_t vt) {
    for (size_t i = 0; i < sizeof program_types / sizeof *program_types; i++)
        if (vt == base + program_types[i].vt) return program_types[i].name;
    return NULL;
}

// --- reading another process ---

// Reads gathered into one process_vm_readv call (at most IOV_MAX = 1024 pieces).
struct batch {
    struct iovec local[1024], remote[1024];
    int n;
    size_t total;
};

static void want(struct batch *b, uint32_t addr, void *dst, size_t len) {
    if (b->n == 1024) fail("too many reads in one batch");
    b->local[b->n] = (struct iovec){dst, len};
    b->remote[b->n] = (struct iovec){(void *)(uintptr_t)addr, len};
    b->n++;
    b->total += len;
}

// 0 when everything arrived. A dangling pointer makes the kernel stop at the first bad piece.
static int fetch(struct batch *b) {
    ssize_t got = b->n ? process_vm_readv(pid, b->local, b->n, b->remote, b->n, 0) : 0;
    int ok = got == (ssize_t)b->total;
    b->n = 0;
    b->total = 0;
    return ok ? 0 : -1;
}

static int rd(uint32_t addr, void *dst, size_t len) {
    struct iovec l = {dst, len}, r = {(void *)(uintptr_t)addr, len};
    return process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)len ? 0 : -1;
}

// --- finding MPC and its Project ---

static pid_t find_mpc(void) {
    DIR *d = opendir("/proc");
    if (!d) die("open /proc");
    pid_t found = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char path[64], exe[64];
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/exe", e->d_name);
        ssize_t n = readlink(path, exe, sizeof exe - 1);
        if (n < 0) continue;
        exe[n] = 0;
        if (!strcmp(exe, "/usr/bin/MPC")) {
            if (found) fail("more than one MPC process");
            found = (pid_t)atoi(e->d_name);
        }
    }
    closedir(d);
    return found;
}

static void check_build(void) {
    char path[64];
    unsigned char id[20];
    snprintf(path, sizeof path, "/proc/%d/exe", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open MPC's executable");
    if (pread(fd, id, sizeof id, BUILD_ID_FILE_OFFSET) != sizeof id) die("read build ID");
    close(fd);
    if (memcmp(id, build_id, sizeof id)) fail("not MPC 3.9.1.2's build ID: no offsets used");
}

struct mapping { uint32_t start, end; char perms[5], path[64]; };
static struct mapping maps[1024];
static int nmaps;

static void read_maps(void) {
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) die("open maps");
    nmaps = 0;
    base = 0;
    while (fgets(line, sizeof line, f) && nmaps < 1024) {
        struct mapping *m = &maps[nmaps];
        unsigned long start, end, off;
        int pos = 0;
        if (sscanf(line, "%lx-%lx %4s %lx %*s %*s%n", &start, &end, m->perms, &off, &pos) != 4) continue;
        m->start = (uint32_t)start;
        m->end = (uint32_t)end;
        m->path[0] = 0;
        sscanf(line + pos, " %63s", m->path);
        if (!strcmp(m->path, "/usr/bin/MPC") && off == 0) base = m->start;
        nmaps++;
    }
    fclose(f);
    if (!base) fail("no load address for /usr/bin/MPC");
}

static int valid_project(const unsigned char *p) {
    if (u32(p, 0) != base + VT_PROJECT) return 0;
    for (size_t i = 0; i < sizeof project_secondary / sizeof *project_secondary; i++)
        if (u32(p, project_secondary[i].off) != base + project_secondary[i].vt) return 0;
    return 1;
}

// Scans the mappings that pick() accepts for the Project's virtual table. Returns the number of
// valid Projects (secondary virtual tables included); the last one goes to *where.
static int scan(int (*pick)(const struct mapping *), uint32_t *where, double *mb) {
    static uint32_t buf[65536];
    uint32_t target = base + VT_PROJECT;
    int valid = 0;
    *mb = 0;
    for (int i = 0; i < nmaps; i++) {
        if (!pick(&maps[i])) continue;
        for (uint32_t a = maps[i].start; a < maps[i].end;) {
            uint32_t len = maps[i].end - a < sizeof buf ? maps[i].end - a : sizeof buf;
            if (rd(a, buf, len)) break;
            for (uint32_t w = 0; w < len / 4; w++) {
                unsigned char p[PROJECT_SIZE];
                if (buf[w] != target) continue;
                if (rd(a + w * 4, p, sizeof p) == 0 && valid_project(p)) {
                    valid++;
                    *where = a + w * 4;
                }
            }
            *mb += len / 1048576.0;
            a += len;
        }
    }
    return valid;
}

static int pick_heap(const struct mapping *m) { return !strcmp(m->path, "[heap]"); }

static int pick_anon(const struct mapping *m) {
    return m->perms[0] == 'r' && m->perms[1] == 'w' && m->path[0] == 0;
}

// Finds MPC and its Project. Returns 0, or -1 when there is no MPC or no single Project.
static int attach(int quiet) {
    pid = find_mpc();
    if (!pid) {
        if (!quiet) fprintf(stderr, "mpcstate: MPC is not running\n");
        return -1;
    }
    check_build();
    read_maps();
    double t0 = now(), mb;
    const char *where = "[heap]";
    int n = scan(pick_heap, &project, &mb);
    if (n == 0) {
        double more;
        where = "anonymous mappings";
        n = scan(pick_anon, &project, &more);
        mb += more;
    }
    if (!quiet || n != 1)
        printf("MPC pid %d, load address 0x%x: %d Project in %s, %.0f MB scanned in %.0f ms\n",
               pid, base, n, where, mb, (now() - t0) * 1000);
    if (n != 1) return -1;
    if (!quiet) printf("Project at 0x%x\n", project);
    return 0;
}

// --- one poll ---

static void copy_name(char *dst, const char *raw) {
    size_t n = strnlen(raw, NAME_LEN - 1);
    memcpy(dst, raw, n);
    dst[n] = 0;
}

// Reads the whole state. Returns 0, or a short reason why this poll can't be trusted.
static const char *poll_state(struct state *s) {
    static unsigned char p[PROJECT_SIZE], pool[POOL_SIZE], seq_head[S_BPM + 4];
    static uint32_t seqs[MAX_SEQS], tracks[MAX_TRACKS];
    static char names[MAX_TRACKS][NAME_LEN], seq_name[NAME_LEN];
    struct batch b = {0};

    want(&b, project, p, sizeof p);
    if (fetch(&b)) return "Project unreadable";
    if (!valid_project(p)) return "Project's virtual tables changed";
    s->sel_track = u32(p, P_SEL_TRACK);
    s->sel_seq = u32(p, P_SEL_SEQ);
    if (s->sel_track != u32(p, P_SEL_TRACK_MIRROR) || s->sel_seq != u32(p, P_SEL_SEQ_MIRROR))
        return "selection being updated";
    uint32_t nseq = u32(p, P_SEQ_COUNT);
    if (nseq == 0 || nseq > u32(p, P_SEQ_CAPACITY) || nseq > MAX_SEQS) return "bad sequence array";

    want(&b, u32(p, P_TRACKPOOL), pool, sizeof pool);
    want(&b, u32(p, P_SEQ_ARRAY), seqs, nseq * 4);
    want(&b, s->sel_seq, seq_head, sizeof seq_head);
    if (fetch(&b)) return "TrackPool or Sequence unreadable";
    if (u32(pool, 0) != base + VT_TRACKPOOL) return "TrackPool's virtual table changed";
    if (u32(seq_head, 0) != base + VT_SEQUENCE) return "Sequence's virtual table changed";
    uint32_t begin = u32(pool, POOL_TRACKS_BEGIN), end = u32(pool, POOL_TRACKS_END);
    uint32_t cap = u32(pool, POOL_TRACKS_CAP);
    if (begin > end || end > cap || (end - begin) % 4 || (end - begin) / 4 > MAX_TRACKS)
        return "bad track vector";
    s->n = (int)((end - begin) / 4);
    s->seq_index = -1;
    for (uint32_t i = 0; i < nseq; i++)
        if (seqs[i] == s->sel_seq) s->seq_index = (int)i;
    if (s->seq_index < 0) return "current Sequence not in the array";
    memcpy(&s->bpm, seq_head + S_BPM, 4);

    if (s->n) want(&b, begin, tracks, (size_t)s->n * 4);
    if (fetch(&b)) return "track vector unreadable";

    uint32_t vt[MAX_TRACKS];
    for (int i = 0; i < s->n; i++) {
        struct track *t = &s->t[i];
        t->addr = tracks[i];
        want(&b, t->addr, &vt[i], 4);
        want(&b, t->addr + T_NAME, &t->name_ptr, 4);
        want(&b, t->addr + T_REC_ARM, &t->arm, 1);
        want(&b, t->addr + T_PROGRAM, &t->program, 4);
    }
    if (fetch(&b)) return "Track unreadable";
    for (int i = 0; i < s->n; i++)
        if (vt[i] != base + VT_TRACK) return "Track's virtual table changed";

    for (int i = 0; i < s->n; i++) {
        struct track *t = &s->t[i];
        want(&b, t->program, &t->program_vt, 4);
        want(&b, t->program + PR_MUTE, &t->mute, 1);
        want(&b, t->program + PR_SOLO, &t->solo, 1);
        want(&b, t->program + PR_SILENCED, &t->silenced, 1);
        want(&b, t->name_ptr, names[i], NAME_LEN);
    }
    want(&b, u32(seq_head, S_NAME), seq_name, NAME_LEN);
    if (fetch(&b)) return "Program or name unreadable";

    s->sel_index = -1;
    for (int i = 0; i < s->n; i++) {
        struct track *t = &s->t[i];
        if (!program_type(t->program_vt)) return "unknown Program class";
        if ((t->arm | t->mute | t->solo | t->silenced) > 1) return "flag not 0 / 1";
        copy_name(t->name, names[i]);
        if (t->addr == s->sel_track) s->sel_index = i;
    }
    copy_name(s->seq_name, seq_name);
    s->sel_index_field = u32(p, P_SEL_TRACK_INDEX);  // printed when it disagrees with the pointer
    return NULL;
}

// --- output ---

static void print_selected(const struct state *s) {
    if (s->sel_index < 0)
        printf("selected track: none of the user tracks (0x%x), index field %u\n", s->sel_track,
               s->sel_index_field);
    else if ((uint32_t)s->sel_index != s->sel_index_field)
        printf("selected track %d, index field %u\n", s->sel_index + 1, s->sel_index_field);
    else
        printf("selected track %d\n", s->sel_index + 1);
}

static void print_tracks(const struct state *s) {
    printf("sequence %d \"%s\" %.2f bpm, %d tracks, ", s->seq_index + 1, s->seq_name, s->bpm, s->n);
    print_selected(s);
    printf("     #  type          mute solo arm silenced  name\n");
    for (int i = 0; i < s->n; i++) {
        const struct track *t = &s->t[i];
        printf("  %c %2d  %-13s %-4s %-4s %-3s %-8s  %s\n", i == s->sel_index ? '>' : ' ', i + 1,
               program_type(t->program_vt), t->mute ? "M" : ".", t->solo ? "S" : ".", t->arm ? "A" : ".",
               t->silenced ? "x" : ".", t->name);
    }
}

static int same_tracks(const struct state *a, const struct state *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++)
        if (a->t[i].addr != b->t[i].addr || a->t[i].program != b->t[i].program) return 0;
    return 1;
}

static void print_changes(const struct state *a, const struct state *b, double t) {
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
        const struct track *x = &a->t[i], *y = &b->t[i];
        static const char *const what[] = {"mute", "solo", "arm", "silenced"};
        const unsigned char was[] = {x->mute, x->solo, x->arm, x->silenced};
        const unsigned char is[] = {y->mute, y->solo, y->arm, y->silenced};
        for (int k = 0; k < 4; k++)
            if (was[k] != is[k]) printf("%9.3f track %2d %-8s %d  (%s)\n", t, i + 1, what[k], is[k], y->name);
        if (strcmp(x->name, y->name)) printf("%9.3f track %2d renamed \"%s\"\n", t, i + 1, y->name);
    }
}

// --- commands ---

static int cmd_show(void) {
    static struct state s;
    if (attach(0)) return 1;
    double t0 = now();
    const char *why = poll_state(&s);
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
    static struct state a, b;
    static double times[1 << 20];
    struct state *prev = &a, *cur = &b;
    long polls = 0, bad = 0, attaches = 0, ticks = 0;
    if (attach(0)) return 1;
    const char *why = poll_state(prev);
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
            if (attach(1) || poll_state(prev)) continue;
            lost = 0;
            last_why = NULL;
            attaches++;
            printf("%9.3f found MPC again (pid %d, Project 0x%x)\n", now() - start, pid, project);
            print_tracks(prev);
            continue;
        }
        double t0 = now();
        why = poll_state(cur);
        double dt = now() - t0;
        if (polls < (long)(sizeof times / sizeof *times)) times[polls] = dt;
        polls++;
        if (why) {
            unsigned char p[PROJECT_SIZE];
            bad++;
            if (why != last_why) printf("%9.3f bad poll: %s\n", t0 - start, why);
            last_why = why;
            // Not a torn read and the Project is gone: look for it again. While it is still there,
            // keep polling it (MPC may be in the middle of a change).
            if (++failing >= 3 && (rd(project, p, sizeof p) || !valid_project(p))) {
                printf("%9.3f lost the Project (%s), searching again\n", t0 - start, why);
                double t1 = now();
                if (attach(1) == 0 && poll_state(prev) == NULL) {
                    attaches++;
                    last_why = NULL;
                    printf("%9.3f Project 0x%x (search took %.0f ms)\n", now() - start, project,
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
        struct state *x = prev;
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

static int cmd_dump(const char *what, int index, size_t len) {
    static struct state s;
    static unsigned char buf[1 << 16];
    if (len == 0 || len > sizeof buf) fail("dump: 1 to 65536 bytes");
    if (!strcmp(what, "at")) {  // a raw address: no need for the Project
        if (!(pid = find_mpc())) fail("MPC is not running");
        if (rd((uint32_t)index, buf, len)) die("read");
        fwrite(buf, 1, len, stdout);
        return 0;
    }
    if (attach(1)) return 1;
    const char *why = poll_state(&s);
    if (why) fail(why);
    uint32_t addr;
    if (!strcmp(what, "project")) {
        addr = project;
    } else {
        if (index < 1 || index > s.n) fail("dump: no such track");
        addr = !strcmp(what, "track") ? s.t[index - 1].addr : s.t[index - 1].program;
    }
    if (rd(addr, buf, len)) die("read");
    fprintf(stderr, "%s at 0x%x, %zu bytes\n", what, addr, len);
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
        return cmd_dump(argv[2], atoi(argv[3]), strtoul(argv[4], NULL, 0));
    if (argc == 5 && !strcmp(argv[1], "dump") && !strcmp(argv[2], "at"))
        return cmd_dump(argv[2], (int)strtoul(argv[3], NULL, 16), strtoul(argv[4], NULL, 0));
    fprintf(stderr, "usage: mpcstate show | watch [secs] [hz] | dump project <bytes> | "
                    "dump track|program <n> <bytes> | dump at <hex address> <bytes>\n");
    return 2;
}
