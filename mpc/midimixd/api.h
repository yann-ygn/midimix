// api: midimixd's client for mpc-tests' native API (the "MPC Native API" addin in MPC, 0.18.0:
// ../mpc/native-api/README.md and WATCH.md) on its Unix socket. A watch subscription keeps the
// model below current (a full snapshot, then changes); commands go out on one short connection
// each and come back as receipts. Everything is non-blocking, driven by the program's poll loop.

#ifndef API_H
#define API_H

#include <poll.h>

#ifndef API_SOCKET  // a test build can point elsewhere
#define API_SOCKET "/tmp/mpc-native-api/api.sock"
#endif
#define API_MAX_TRACKS 136  // the API's own limit: 128 MIDI-triggered + 8 audio tracks
#define API_MAX_FDS    7    // the watch and up to 6 commands
#define API_ID_LEN     40   // instance "run_<32 hex>", or a decimal object ID
#define API_SENDS      4    // sends A-D
#define API_INSERTS    4    // insert slots 1-4
#define API_PRODUCT_LEN 256  // the API's bound for an instrument's productId, NUL included

struct api_track {
    char id[API_ID_LEN];         // trackId: what commands name
    char mixer_gen[API_ID_LEN];  // mixer.generation; "" when the API can't set this track's mixer
    char name[64], type[16];
    signed char mute, solo, arm, silenced;  // 1, 0, or -1 when the API can't read it on this track
    float volume;                           // normalized 0-1 (the fader's position), or -1
    float pan;                              // normalized 0-1, 0.5 = centre, or -1
    float send[API_SENDS];                  // normalized 0-1, or -1
    signed char insert[API_INSERTS];        // an occupied slot's enable: 1, 0, or -1 (empty, not readable)
    long colour;  // RGB24 (setup.colour), -1 unknown; only full snapshots have it: changes keep the last
    // instrument.productId of the plugin on a plugin track ("MPC:Hype", "VST:<UID>:<library path>"), ""
    // when the API names none. Like colour, only full snapshots have it.
    char product[API_PRODUCT_LEN];
};

struct api_state {
    int live;  // a snapshot is applied and the watch is current
    char instance[API_ID_LEN], project[API_ID_LEN];  // project "" when MPC has none open
    char mute_scope;                                 // 'g' global, 's' per sequence, 0 unknown
    char seq_id[API_ID_LEN], seq_gen[API_ID_LEN];    // the active sequence, for sequence mutes
    char selected[API_ID_LEN];                       // the selected track's trackId, "" none
    int n;
    struct api_track t[API_MAX_TRACKS];  // every user track, in MPC's Track List order
};

extern struct api_state api;

enum api_op { API_MUTE, API_SOLO, API_ARM, API_VOLUME, API_PAN, API_SEND, API_INSERT, API_SELECT };

struct api_result {
    int id;  // what api_send returned
    enum api_op op;
    int index;  // API_SEND's send, API_INSERT's slot (0-3)
    char track[API_ID_LEN];
    const char *status;  // applied, rejected, cancelled, unknown, accepted, or "failed": no receipt
    const char *error;   // the API's error code, or what went wrong here; NULL if none
    int total_ms;        // the API's acceptance-to-receipt time, -1 if not given
};

// Provided by the program: a log line, whether to log every frame, a monotonic clock in seconds.
void say(const char *fmt, ...);
extern int verbose;
double now(void);

void api_init(void (*on_result)(const struct api_result *));
// Sends a command to track t (an index in api.t): value 0 / 1 for the flags and API_INSERT, 0-1 for
// API_VOLUME, API_PAN and API_SEND; index: the send or insert slot (0-3). expected 0 / 1: the flag's
// value MPC must still have when the command runs, or -1. API_SELECT selects the track (no value).
// Levels go as the API's continuous kind: each control's receipt replaces its last one, apart from the
// receipts the other commands keep. The API refuses one with QUEUE_FULL or RETENTION_CAPACITY when
// that kind is busy (nothing ran).
// Returns an id > 0, passed back once with the result; -1 if nothing was sent.
int api_send(enum api_op op, int t, int index, double value, int expected);
// The sockets to poll (POLLIN), and what to do with their events.
int api_pollfds(struct pollfd *fds, int max);
void api_events(const struct pollfd *fds, int n);
// Timers: connecting and reconnecting the watch, giving up on silent connections.
void api_tick(double t);
void api_close(void);

#endif
