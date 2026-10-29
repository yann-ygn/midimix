// ctl: midimixd's control socket, for the editor plugin (mpc/plugin) and midimixctl. Root only (0600),
// like the native API's. One request per line, each answered with lines that end with an empty line:
//
//   get          the state (midimixd.c's ctl_state)
//   map <spec>   this connection's mapping (map.h), rows not named taking the default's. It is in effect
//                while the connection is open, the latest one set among the open ones winning; once none
//                is left, the default is. "ok", or "error <why>"
//   unmap        drops this connection's mapping: "ok"
//
// So the plugin's mapping lasts as long as the plugin instance (its project), with nothing to undo.

#ifndef CTL_H
#define CTL_H

#include <poll.h>
#include <stddef.h>

#ifndef CTL_SOCKET  // a test build can point elsewhere
#define CTL_SOCKET "/run/midimixd.sock"
#endif
#define CTL_MAX_FDS 9  // the listening socket and 8 clients

// Listens on CTL_SOCKET. 0, or -1 (logged): midimixd carries on without it.
int ctl_init(void);
int ctl_pollfds(struct pollfd *fds, int max);
void ctl_events(const struct pollfd *fds, int n);
// The mapping in effect: the latest one set by an open connection, else map_default. *owner: that
// connection's number, -1 for the default.
const unsigned char *ctl_map(int *owner);
void ctl_close(void);

// Provided by the program: the state for a get from connection c, in buf (cap bytes, ending with the
// empty line). Its length.
size_t ctl_state(int c, char *buf, size_t cap);

#endif
