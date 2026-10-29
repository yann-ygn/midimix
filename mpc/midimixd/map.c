// map: see map.h.

#include "map.h"

#include <stdio.h>
#include <string.h>

const char *const map_row_name[MAP_ROWS] = {"top", "mid", "bottom", "fader", "mute", "solo", "arm"};
const char *const map_target_name[MAP_TARGETS] = {
    "none", "volume", "pan", "send1", "send2", "send3", "send4",
    "mute", "solo", "arm", "insert1", "insert2", "insert3", "insert4", "select",
};
const unsigned char map_default[MAP_ROWS] = {T_NONE, T_NONE, T_PAN, T_VOLUME, T_MUTE, T_SOLO, T_ARM};

const unsigned char map_level_targets[MAP_LEVEL_TARGETS] = {T_NONE, T_VOLUME, T_PAN, T_SEND1, T_SEND2, T_SEND3, T_SEND4};
const unsigned char map_button_targets[MAP_BUTTON_TARGETS] = {
    T_NONE, T_MUTE, T_SOLO, T_ARM, T_INSERT1, T_INSERT2, T_INSERT3, T_INSERT4, T_SELECT,
};

int map_allowed(int row, int target) {
    if (row < 0 || row >= MAP_ROWS || target < 0 || target >= MAP_TARGETS) return 0;
    if (target == T_NONE) return 1;
    return row < MAP_LEVEL_ROWS ? target <= T_SEND4 : target >= T_MUTE;
}

static int lookup(const char *const *names, int n, const char *s, size_t len) {
    for (int i = 0; i < n; i++)
        if (strlen(names[i]) == len && !strncmp(names[i], s, len)) return i;
    return -1;
}

int map_parse(unsigned char map[MAP_ROWS], const char *spec, char *err, size_t errlen) {
    unsigned char m[MAP_ROWS];
    memcpy(m, map, sizeof m);
    for (const char *p = spec;;) {
        p += strspn(p, " \t");
        if (!*p) break;
        size_t len = strcspn(p, " \t");
        const char *eq = memchr(p, '=', len);
        if (!eq) {
            snprintf(err, errlen, "expected row=target, got \"%.*s\"", (int)len, p);
            return -1;
        }
        int row = lookup(map_row_name, MAP_ROWS, p, (size_t)(eq - p));
        int target = lookup(map_target_name, MAP_TARGETS, eq + 1, len - (size_t)(eq + 1 - p));
        if (row < 0 || target < 0 || !map_allowed(row, target)) {
            if (row < 0) snprintf(err, errlen, "no row \"%.*s\"", (int)(eq - p), p);
            else if (target < 0) snprintf(err, errlen, "no target \"%.*s\"", (int)(len - (size_t)(eq + 1 - p)), eq + 1);
            else snprintf(err, errlen, "%s can't be %s", map_row_name[row], map_target_name[target]);
            return -1;
        }
        m[row] = (unsigned char)target;
        p += len;
    }
    memcpy(map, m, sizeof m);
    return 0;
}

int map_format(const unsigned char map[MAP_ROWS], char *buf, size_t len) {
    int n = 0;
    if (len) buf[0] = '\0';
    for (int r = 0; r < MAP_ROWS; r++) {
        int t = map[r] < MAP_TARGETS ? map[r] : T_NONE;
        size_t at = (size_t)n < len ? (size_t)n : len;
        n += snprintf(buf + at, len - at, "%s%s=%s", r ? " " : "", map_row_name[r], map_target_name[t]);
    }
    return n;
}
