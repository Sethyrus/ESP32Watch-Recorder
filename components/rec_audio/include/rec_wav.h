#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

// Canonical 44-byte PCM WAV header (RIFF, "fmt " chunk of 16 bytes, "data").
#define REC_WAV_HEADER_BYTES 44

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_offset; // first sample byte
    uint32_t data_bytes;  // as found, clamped to the file size
} rec_wav_info_t;

// Fills out[REC_WAV_HEADER_BYTES] for 16-bit PCM.
void rec_wav_header(uint8_t *out, uint32_t sample_rate, uint16_t channels, uint32_t data_bytes);

// Parses the header of an open file (any chunk layout); only 16-bit PCM is accepted.
esp_err_t rec_wav_read_info(FILE *f, rec_wav_info_t *out);

// Rewrites the sizes of a canonical header left at 0 or short by a power cut. True if
// the file was changed.
bool rec_wav_repair(const char *path);
