#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

// Recordings live in REC_DIR on the microSD, one WAV per recording, named after the
// start time (YYYYMMDD_HHMMSS.wav), so sorting names sorts them by date.
#define REC_DIR "/sdcard/rec"
#define REC_NAME_MAX 32

typedef struct {
    char name[REC_NAME_MAX]; // file name inside REC_DIR
    uint32_t bytes;          // file size
    uint32_t duration_ms;    // from the size, at the recorder's format
} rec_file_t;

// Mounts the card (BSP SDMMC 1-bit, never formats), creates REC_DIR and repairs the
// headers of recordings cut by a power loss. Safe to call again after a failure.
esp_err_t rec_files_mount(void);
bool rec_files_mounted(void);

// Newest first. Returns how many were written to out (at most max); *total, if not
// NULL, gets the number of recordings on the card.
int rec_files_list(rec_file_t *out, int max, int *total);

esp_err_t rec_files_delete(const char *name);

// Free and total bytes of the card; false if not mounted.
bool rec_files_space(uint64_t *free_bytes, uint64_t *total_bytes);

// REC_DIR "/" name.
void rec_files_path(const char *name, char *out, size_t size);

// "30 sep · 14:30" from a YYYYMMDD_HHMMSS name; the name itself otherwise.
void rec_files_title(const char *name, char *out, size_t size);
