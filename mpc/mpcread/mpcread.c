// mpcread: see mpcread.h. Each poll is 5 process_vm_readv calls, one per level of pointers:
// Project, TrackPool and the current Sequence, the track vector, the Tracks, then their Programs
// and names.

#define _GNU_SOURCE
#include "mpcread.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define PR_VOLUME           0x500  // copies at +0x504, +0x510, +0x51c, +0x520, +0x52c, +0x5ac

#define MAX_SEQS 2048

pid_t mpc_pid;
uint32_t mpc_base, mpc_project;

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

const char *mpc_program_type(uint32_t vt) {
    for (size_t i = 0; i < sizeof program_types / sizeof *program_types; i++)
        if (vt == mpc_base + program_types[i].vt) return program_types[i].name;
    return NULL;
}

// --- reading another process ---

// Reads gathered into one process_vm_readv call (at most IOV_MAX = 1024 pieces).
struct batch {
    struct iovec local[1024], remote[1024];
    int n, overflow;
    size_t total;
};

static void want(struct batch *b, uint32_t addr, void *dst, size_t len) {
    if (b->n == 1024) {
        b->overflow = 1;
        return;
    }
    b->local[b->n] = (struct iovec){dst, len};
    b->remote[b->n] = (struct iovec){(void *)(uintptr_t)addr, len};
    b->n++;
    b->total += len;
}

// 0 when everything arrived. A dangling pointer makes the kernel stop at the first bad piece.
static int fetch(struct batch *b) {
    ssize_t got = b->n && !b->overflow ? process_vm_readv(mpc_pid, b->local, b->n, b->remote, b->n, 0) : 0;
    int ok = !b->overflow && got == (ssize_t)b->total;
    b->n = b->overflow = 0;
    b->total = 0;
    return ok ? 0 : -1;
}

int mpc_read(uint32_t addr, void *dst, size_t len) {
    struct iovec l = {dst, len}, r = {(void *)(uintptr_t)addr, len};
    return process_vm_readv(mpc_pid, &l, 1, &r, 1, 0) == (ssize_t)len ? 0 : -1;
}

// --- finding MPC and its Project ---

pid_t mpc_find_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
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
            if (found) {
                found = -1;
                break;
            }
            found = (pid_t)atoi(e->d_name);
        }
    }
    closedir(d);
    return found;
}

static const char *check_build(void) {
    char path[64];
    unsigned char id[20];
    snprintf(path, sizeof path, "/proc/%d/exe", mpc_pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return "can't open MPC's executable";
    ssize_t got = pread(fd, id, sizeof id, BUILD_ID_FILE_OFFSET);
    close(fd);
    if (got != sizeof id) return "can't read MPC's build ID";
    if (memcmp(id, build_id, sizeof id)) return "not MPC 3.9.1.2's build ID: no offsets used";
    return NULL;
}

struct mapping { uint32_t start, end; char perms[5], path[64]; };
static struct mapping maps[1024];
static int nmaps;

static const char *read_maps(void) {
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/maps", mpc_pid);
    FILE *f = fopen(path, "r");
    if (!f) return "can't open MPC's memory map";
    nmaps = 0;
    mpc_base = 0;
    while (fgets(line, sizeof line, f) && nmaps < 1024) {
        struct mapping *m = &maps[nmaps];
        unsigned long start, end, off;
        int pos = 0;
        if (sscanf(line, "%lx-%lx %4s %lx %*s %*s%n", &start, &end, m->perms, &off, &pos) != 4) continue;
        m->start = (uint32_t)start;
        m->end = (uint32_t)end;
        m->path[0] = 0;
        sscanf(line + pos, " %63s", m->path);
        if (!strcmp(m->path, "/usr/bin/MPC") && off == 0) mpc_base = m->start;
        nmaps++;
    }
    fclose(f);
    return mpc_base ? NULL : "no load address for /usr/bin/MPC";
}

static int valid_project(const unsigned char *p) {
    if (u32(p, 0) != mpc_base + VT_PROJECT) return 0;
    for (size_t i = 0; i < sizeof project_secondary / sizeof *project_secondary; i++)
        if (u32(p, project_secondary[i].off) != mpc_base + project_secondary[i].vt) return 0;
    return 1;
}

// Scans the mappings that pick() accepts for the Project's virtual table. Returns the number of
// valid Projects (secondary virtual tables included); the last one goes to *where.
static int scan(int (*pick)(const struct mapping *), uint32_t *where, double *mb) {
    static uint32_t buf[65536];
    uint32_t target = mpc_base + VT_PROJECT;
    int valid = 0;
    for (int i = 0; i < nmaps; i++) {
        if (!pick(&maps[i])) continue;
        for (uint32_t a = maps[i].start; a < maps[i].end;) {
            uint32_t len = maps[i].end - a < sizeof buf ? maps[i].end - a : sizeof buf;
            if (mpc_read(a, buf, len)) break;
            for (uint32_t w = 0; w < len / 4; w++) {
                unsigned char p[PROJECT_SIZE];
                if (buf[w] != target) continue;
                if (mpc_read(a + w * 4, p, sizeof p) == 0 && valid_project(p)) {
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

const char *mpc_attach(struct mpc_found *f) {
    memset(f, 0, sizeof *f);
    mpc_project = 0;
    pid_t pid = mpc_find_pid();
    if (pid < 0) return "more than one MPC process";
    if (!pid) return "MPC is not running";
    mpc_pid = f->pid = pid;
    const char *why = check_build();
    if (!why) why = read_maps();
    if (why) return why;
    f->base = mpc_base;

    double t0 = now();
    uint32_t project = 0;
    f->where = "[heap]";
    f->projects = scan(pick_heap, &project, &f->mb);
    if (f->projects == 0) {
        f->where = "anonymous mappings";
        f->projects = scan(pick_anon, &project, &f->mb);
    }
    f->ms = (now() - t0) * 1000;
    if (f->projects == 0) return "no Project in MPC's memory";
    if (f->projects > 1) return "more than one Project in MPC's memory";
    mpc_project = f->project = project;
    return NULL;
}

int mpc_project_alive(void) {
    unsigned char p[PROJECT_SIZE];
    return mpc_project && mpc_read(mpc_project, p, sizeof p) == 0 && valid_project(p);
}

// --- one poll ---

static void copy_name(char *dst, const char *raw) {
    size_t n = strnlen(raw, MPC_NAME_LEN - 1);
    memcpy(dst, raw, n);
    dst[n] = 0;
}

const char *mpc_poll(struct mpc_state *s) {
    static unsigned char p[PROJECT_SIZE], pool[POOL_SIZE], seq_head[S_BPM + 4];
    static uint32_t seqs[MAX_SEQS], tracks[MPC_MAX_TRACKS], vt[MPC_MAX_TRACKS];
    static char names[MPC_MAX_TRACKS][MPC_NAME_LEN], seq_name[MPC_NAME_LEN];
    static struct batch b;

    if (!mpc_project) return "no Project";
    want(&b, mpc_project, p, sizeof p);
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
    if (u32(pool, 0) != mpc_base + VT_TRACKPOOL) return "TrackPool's virtual table changed";
    if (u32(seq_head, 0) != mpc_base + VT_SEQUENCE) return "Sequence's virtual table changed";
    uint32_t begin = u32(pool, POOL_TRACKS_BEGIN), end = u32(pool, POOL_TRACKS_END);
    uint32_t cap = u32(pool, POOL_TRACKS_CAP);
    if (begin > end || end > cap || (end - begin) % 4) return "bad track vector";
    if ((end - begin) / 4 > MPC_MAX_TRACKS) return "more tracks than MPC allows";
    s->n = (int)((end - begin) / 4);
    s->seq_index = -1;
    for (uint32_t i = 0; i < nseq; i++)
        if (seqs[i] == s->sel_seq) s->seq_index = (int)i;
    if (s->seq_index < 0) return "current Sequence not in the array";
    memcpy(&s->bpm, seq_head + S_BPM, 4);

    if (s->n) want(&b, begin, tracks, (size_t)s->n * 4);
    if (fetch(&b)) return "track vector unreadable";

    for (int i = 0; i < s->n; i++) {
        struct mpc_track *t = &s->t[i];
        t->addr = tracks[i];
        want(&b, t->addr, &vt[i], 4);
        want(&b, t->addr + T_NAME, &t->name_ptr, 4);
        want(&b, t->addr + T_REC_ARM, &t->arm, 1);
        want(&b, t->addr + T_PROGRAM, &t->program, 4);
    }
    if (fetch(&b)) return "Track unreadable";
    for (int i = 0; i < s->n; i++)
        if (vt[i] != mpc_base + VT_TRACK) return "Track's virtual table changed";

    for (int i = 0; i < s->n; i++) {
        struct mpc_track *t = &s->t[i];
        want(&b, t->program, &t->program_vt, 4);
        want(&b, t->program + PR_MUTE, &t->mute, 1);
        want(&b, t->program + PR_SOLO, &t->solo, 1);
        want(&b, t->program + PR_SILENCED, &t->silenced, 1);
        want(&b, t->program + PR_VOLUME, &t->volume, 4);
        want(&b, t->name_ptr, names[i], MPC_NAME_LEN);
    }
    want(&b, u32(seq_head, S_NAME), seq_name, MPC_NAME_LEN);
    if (fetch(&b)) return "Program or name unreadable";

    s->sel_index = -1;
    for (int i = 0; i < s->n; i++) {
        struct mpc_track *t = &s->t[i];
        if (!mpc_program_type(t->program_vt)) return "unknown Program class";
        if ((t->arm | t->mute | t->solo | t->silenced) > 1) return "flag not 0 / 1";
        copy_name(t->name, names[i]);
        if (t->addr == s->sel_track) s->sel_index = i;
    }
    copy_name(s->seq_name, seq_name);
    s->sel_index_field = u32(p, P_SEL_TRACK_INDEX);  // lags the pointer by one poll
    return NULL;
}
