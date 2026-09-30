#pragma once

#include "rec_files.h"
#include "rec_ui.h"

extern const rec_screen_t rec_record_screen; // root: record / stop / pause
extern const rec_screen_t rec_list_screen;   // recordings, newest first
extern const rec_screen_t rec_player_screen; // play, seek, volume, delete
extern const rec_screen_t rec_delete_screen; // confirmation

// Opens the player on a recording and starts playing it.
void rec_player_open(const rec_file_t *file);

// Speaker volume: 5 levels, kept in NVS (namespace "recorder").
#define REC_VOLUME_LEVELS 5
int rec_volume_level(void); // 1..REC_VOLUME_LEVELS
void rec_volume_step(void); // next level, wrapping to 1
