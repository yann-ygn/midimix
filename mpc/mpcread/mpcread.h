// mpcread: read every track's mute / solo / rec arm / volume from the running MPC app's memory.
// Read-only: process_vm_readv as root, no ptrace stop, no writes, no calls into MPC. Method and
// offsets come from mpc-tests (../mpc/findings/live-state.md) and only fit MPC 3.9.1.2: the
// executable's GNU build ID is checked before any offset is used. Results in CLAUDE.md ("Reading
// MPC's state from memory"). Used by mpcstate (the command-line reader); midimixd used it until it
// moved onto mpc-tests' native API.

#ifndef MPCREAD_H
#define MPCREAD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MPC_MAX_TRACKS 128  // MPC's own limit; also keeps each poll's batches under IOV_MAX
#define MPC_NAME_LEN   48

struct mpc_track {
    uint32_t addr, program, name_ptr, program_vt;
    unsigned char arm, mute, solo, silenced;  // silenced: by its own mute or another track's solo
    float volume;                             // linear gain, 1.0 = 0 dB (not checked on MIDI / CV)
    char name[MPC_NAME_LEN];
};

struct mpc_state {
    uint32_t sel_track, sel_seq, sel_index_field;
    int sel_index, seq_index, n;  // sel_index -1: the selected track is not a user track
    float bpm;
    char seq_name[MPC_NAME_LEN];
    struct mpc_track t[MPC_MAX_TRACKS];  // every user track, in the UI's order
};

// What mpc_attach found, for logging.
struct mpc_found {
    pid_t pid;
    uint32_t base, project;
    int projects;       // valid Projects the scan found
    const char *where;  // the mappings scanned
    double mb, ms;
};

extern pid_t mpc_pid;
extern uint32_t mpc_base, mpc_project;

// The running MPC's pid; 0 if there is none, -1 if there are several.
pid_t mpc_find_pid(void);
// Finds MPC, checks its build and scans its heap for the Project (~0.4 s). Returns NULL, or why
// it couldn't; *found says what it saw either way.
const char *mpc_attach(struct mpc_found *found);
// Reads the whole state (5 process_vm_readv calls). Returns NULL, or a short reason why this read
// can't be trusted: MPC may be in the middle of a change, so try again before giving up.
const char *mpc_poll(struct mpc_state *s);
// 1 while the Project mpc_attach found is still there (its virtual tables still match).
int mpc_project_alive(void);
// "plugin", "drum" ... or NULL for a Program class this build's table doesn't know.
const char *mpc_program_type(uint32_t vt);
// A raw read from MPC's memory: 0, or -1 with errno set.
int mpc_read(uint32_t addr, void *dst, size_t len);

#endif
