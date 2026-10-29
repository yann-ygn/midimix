// map: what each row of the Midimix's strips does, the same for the 8 strips, and its text form. Shared
// by midimixd and the editor plugin (mpc/plugin), which keeps it in its project:
//
//   top=none mid=none bottom=pan fader=volume mute=mute solo=solo arm=arm
//
// Rows, top to bottom: the three knobs, the fader (level rows), then the buttons: mute, solo (the mute
// buttons while SOLO is held) and rec arm. Level rows take none, volume, pan or send1-send4; button
// rows none, mute, solo, arm, insert1-insert4 (an insert slot's on / off) or select (selects the track).

#ifndef MAP_H
#define MAP_H

#include <stddef.h>

enum map_row { ROW_TOP, ROW_MID, ROW_BOTTOM, ROW_FADER, ROW_MUTE, ROW_SOLO, ROW_ARM, MAP_ROWS };
#define MAP_LEVEL_ROWS 4  // ROW_TOP to ROW_FADER; the rest are buttons

enum map_target {
    T_NONE,
    T_VOLUME, T_PAN, T_SEND1, T_SEND2, T_SEND3, T_SEND4,  // levels
    T_MUTE, T_SOLO, T_ARM, T_INSERT1, T_INSERT2, T_INSERT3, T_INSERT4, T_SELECT,  // buttons
    MAP_TARGETS
};

#define MAP_SPEC_MAX 160  // the longest spec map_format writes, NUL included

extern const char *const map_row_name[MAP_ROWS];
extern const char *const map_target_name[MAP_TARGETS];
extern const unsigned char map_default[MAP_ROWS];

// The targets a level row or a button row takes, in the editor's order (T_NONE first).
#define MAP_LEVEL_TARGETS  7
#define MAP_BUTTON_TARGETS 9
extern const unsigned char map_level_targets[MAP_LEVEL_TARGETS];
extern const unsigned char map_button_targets[MAP_BUTTON_TARGETS];

// 1 if row takes target.
int map_allowed(int row, int target);
// Reads "row=target ..." into map, whose other rows stay as they are. 0, or -1 with why in err
// (map unchanged).
int map_parse(unsigned char map[MAP_ROWS], const char *spec, char *err, size_t errlen);
// Writes every row: the length, as snprintf.
int map_format(const unsigned char map[MAP_ROWS], char *buf, size_t len);

#endif
