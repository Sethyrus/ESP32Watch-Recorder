#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Voice recorder engine: mic (ES7210, both mics mixed to mono) -> PSRAM ring buffer
// -> WAV on the microSD, and WAV -> speaker (ES8311). Recording and playback never
// run at the same time. The calls below are cheap and safe from any task.

#define REC_SAMPLE_RATE 16000
#define REC_BYTES_PER_S (REC_SAMPLE_RATE * 2) // 16-bit mono

typedef enum {
    REC_IDLE = 0,
    REC_RECORDING,
    REC_PAUSED,
    REC_SAVING, // stop requested, the writer is flushing the buffer
} rec_state_t;

typedef enum {
    REC_END_NONE = 0,
    REC_END_USER,  // rec_record_stop()
    REC_END_FULL,  // the card ran out of space
    REC_END_ERROR, // mic or SD error
    REC_END_SHORT, // under 1 s: discarded
} rec_end_t;

typedef enum {
    REC_PLAY_STOPPED = 0,
    REC_PLAY_PLAYING,
    REC_PLAY_PAUSED,
    REC_PLAY_ENDED,
    REC_PLAY_FAILED, // file unreadable or not 16-bit PCM
} rec_play_state_t;

// Creates the tasks and buffers; the codecs are opened only while in use.
esp_err_t rec_audio_init(void);

// Recording or playing: the chip must stay awake (no light sleep).
bool rec_audio_busy(void);

// ---- Recording ----
// Starts a new recording named after the current time. ESP_ERR_INVALID_STATE: no
// card; ESP_ERR_NO_MEM: card full; ESP_FAIL: file or mic error.
esp_err_t rec_record_start(void);
void rec_record_set_paused(bool paused);
void rec_record_stop(void); // asynchronous: state goes SAVING, then IDLE
rec_state_t rec_record_state(void);
uint32_t rec_record_elapsed_ms(void); // audio kept so far (pauses excluded)
int rec_record_level(void);           // input level 0..100 (log scale), live
// How the last recording ended, once (REC_END_NONE if not yet read or none).
rec_end_t rec_record_take_end(void);

// ---- Playback ----
esp_err_t rec_play_start(const char *name); // file in REC_DIR, from the start
void rec_play_toggle(void);                 // pause/resume; restarts when ended
void rec_play_seek(int32_t delta_ms);
void rec_play_stop(void); // waits until the speaker is closed
rec_play_state_t rec_play_state(void);
uint32_t rec_play_position_ms(void);
uint32_t rec_play_duration_ms(void);
void rec_play_set_volume(int percent); // speaker volume 0..100
