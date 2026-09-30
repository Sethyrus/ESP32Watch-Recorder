#include "rec_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "rec_files.h"
#include "rec_wav.h"

#define MIC_GAIN_DB 37.5f // ES7210 maximum; speech at arm's length is still ~-30 dBFS
#define DIGITAL_GAIN 2      // +6 dB on the mono mix, saturated
#define FRAME_SAMPLES 320                   // 20 ms at 16 kHz
#define RING_BYTES (256 * 1024)             // ~8 s of audio against slow SD writes
#define WRITE_BYTES (16 * 1024)             // SD write block
#define HEADER_PERIOD_US (10 * 1000000LL)   // header + fsync, so a power cut loses little
#define SPACE_PERIOD_US (5 * 1000000LL)
#define MIN_FREE_BYTES (5ULL * 1024 * 1024) // stop before the card is completely full
#define MIN_KEEP_BYTES REC_BYTES_PER_S      // shorter recordings are discarded
#define PLAY_CHUNK 2048

static const char *TAG = "rec_audio";

static esp_codec_dev_handle_t s_mic;
static esp_codec_dev_handle_t s_speaker;
static StreamBufferHandle_t s_ring;
static TaskHandle_t s_capture_task;
static TaskHandle_t s_writer_task;
static TaskHandle_t s_play_task;

// Recording
static volatile rec_state_t s_rec_state;
static volatile bool s_rec_stop;      // capture ends its loop
static volatile bool s_capture_done;  // capture closed the mic
static volatile uint32_t s_rec_samples;
static volatile int s_level;
static volatile rec_end_t s_rec_end;
static volatile rec_end_t s_rec_end_pending; // reported by rec_record_take_end
static FILE *s_rec_file;
static char s_rec_path[64];

// Playback
static volatile rec_play_state_t s_play_state;
static volatile bool s_play_active; // the play task owns a file
static volatile bool s_play_stop;
static volatile bool s_play_toggle;
static volatile int32_t s_play_seek_ms;
static volatile uint32_t s_play_pos_ms;
static volatile uint32_t s_play_dur_ms;
static volatile int s_volume = 70;
static volatile bool s_volume_dirty;
static char s_play_path[64];

// ---------- recording ----------

static int level_from_rms(float rms)
{
    // -60..0 dBFS -> 0..100
    const float db = rms > 1.0f ? 20.0f * log10f(rms / 32768.0f) : -96.0f;
    const int v = (int)((db + 60.0f) * 100.0f / 60.0f);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static void capture_task(void *arg)
{
    static int16_t stereo[FRAME_SAMPLES * 2];
    static int16_t mono[FRAME_SAMPLES];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t overflows = 0;
        int64_t next_log = esp_timer_get_time() + 1000000;
        double sum_l = 0, sum_r = 0;
        int frames = 0;
        while (!s_rec_stop) {
            if (esp_codec_dev_read(s_mic, stereo, sizeof(stereo)) != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "Mic read failed");
                s_rec_end = REC_END_ERROR;
                break;
            }
            double sum = 0;
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                const int l = stereo[2 * i], r = stereo[2 * i + 1];
                int m = (l + r) * DIGITAL_GAIN / 2;
                m = m > INT16_MAX ? INT16_MAX : m < INT16_MIN ? INT16_MIN : m;
                mono[i] = (int16_t)m;
                sum += (double)m * m;
                sum_l += (double)l * l;
                sum_r += (double)r * r;
            }
            frames++;
            s_level = level_from_rms(sqrtf((float)(sum / FRAME_SAMPLES)));
            if (s_rec_state == REC_RECORDING) {
                const size_t sent = xStreamBufferSend(s_ring, mono, sizeof(mono), 0);
                if (sent < sizeof(mono)) {
                    overflows++;
                }
                s_rec_samples += sent / 2;
            }
            const int64_t now = esp_timer_get_time();
            if (now >= next_log) {
                next_log = now + 1000000;
                const float n = (float)frames * FRAME_SAMPLES;
                ESP_LOGI(TAG, "mic L %.0f R %.0f rms, level %d, %u s, %u overflows", sqrt(sum_l / n),
                         sqrt(sum_r / n), s_level, (unsigned)(s_rec_samples / REC_SAMPLE_RATE),
                         (unsigned)overflows);
                sum_l = sum_r = 0;
                frames = 0;
            }
        }
        esp_codec_dev_close(s_mic);
        s_level = 0;
        s_capture_done = true; // the writer polls this every 100 ms
    }
}

static void write_header(void)
{
    uint8_t h[REC_WAV_HEADER_BYTES];
    const long end = ftell(s_rec_file);
    const uint32_t data = end > REC_WAV_HEADER_BYTES ? (uint32_t)(end - REC_WAV_HEADER_BYTES) : 0;
    rec_wav_header(h, REC_SAMPLE_RATE, 1, data);
    fseek(s_rec_file, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), s_rec_file);
    fseek(s_rec_file, end, SEEK_SET);
}

static void writer_task(void *arg)
{
    static uint8_t block[WRITE_BYTES]; // internal RAM: the SDMMC DMA reads it directly
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_rec_file == NULL) {
            continue;
        }
        uint32_t written = 0;
        int64_t next_header = esp_timer_get_time() + HEADER_PERIOD_US;
        int64_t next_space = esp_timer_get_time() + SPACE_PERIOD_US;
        for (;;) {
            const size_t n = xStreamBufferReceive(s_ring, block, sizeof(block), pdMS_TO_TICKS(100));
            if (n > 0) {
                if (fwrite(block, 1, n, s_rec_file) != n) {
                    ESP_LOGE(TAG, "SD write failed after %u bytes", (unsigned)written);
                    s_rec_end = REC_END_ERROR;
                    s_rec_stop = true;
                } else {
                    written += n;
                }
            }
            const int64_t now = esp_timer_get_time();
            if (now >= next_header) {
                next_header = now + HEADER_PERIOD_US;
                write_header();
                fflush(s_rec_file);
                fsync(fileno(s_rec_file));
            }
            if (now >= next_space) {
                next_space = now + SPACE_PERIOD_US;
                uint64_t free_bytes, total;
                if (rec_files_space(&free_bytes, &total) && free_bytes < MIN_FREE_BYTES) {
                    ESP_LOGW(TAG, "Card full, stopping");
                    s_rec_end = REC_END_FULL;
                    s_rec_stop = true;
                }
            }
            if (s_rec_stop && s_rec_state != REC_SAVING) {
                s_rec_state = REC_SAVING;
            }
            if (s_capture_done && xStreamBufferIsEmpty(s_ring)) {
                break;
            }
        }
        write_header();
        fclose(s_rec_file);
        s_rec_file = NULL;
        if (written < MIN_KEEP_BYTES && s_rec_end == REC_END_USER) {
            unlink(s_rec_path);
            s_rec_end = REC_END_SHORT;
            ESP_LOGI(TAG, "Recording under 1 s discarded");
        } else {
            ESP_LOGI(TAG, "Saved %s: %u bytes (%.1f s)", s_rec_path, (unsigned)written,
                     written / (float)REC_BYTES_PER_S);
        }
        s_rec_end_pending = s_rec_end;
        s_rec_state = REC_IDLE;
    }
}

esp_err_t rec_record_start(void)
{
    if (s_rec_state != REC_IDLE) {
        return ESP_OK;
    }
    if (rec_files_mount() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    rec_play_stop();
    uint64_t free_bytes = 0, total = 0;
    if (!rec_files_space(&free_bytes, &total) || free_bytes < MIN_FREE_BYTES) {
        return ESP_ERR_NO_MEM;
    }

    char name[REC_NAME_MAX];
    const time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(name, sizeof(name), "%Y%m%d_%H%M%S", &tm);
    struct stat st;
    snprintf(s_rec_path, sizeof(s_rec_path), "%s/%s.wav", REC_DIR, name);
    for (int i = 2; stat(s_rec_path, &st) == 0 && i < 100; i++) {
        snprintf(s_rec_path, sizeof(s_rec_path), "%s/%s_%d.wav", REC_DIR, name, i);
    }
    s_rec_file = fopen(s_rec_path, "wb");
    if (s_rec_file == NULL) {
        ESP_LOGE(TAG, "Cannot create %s", s_rec_path);
        return ESP_FAIL;
    }
    uint8_t h[REC_WAV_HEADER_BYTES];
    rec_wav_header(h, REC_SAMPLE_RATE, 1, 0);
    fwrite(h, 1, sizeof(h), s_rec_file);

    if (s_mic == NULL) {
        s_mic = bsp_audio_codec_microphone_init();
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = REC_SAMPLE_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (s_mic == NULL || esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Mic unavailable");
        fclose(s_rec_file);
        s_rec_file = NULL;
        unlink(s_rec_path);
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);

    xStreamBufferReset(s_ring);
    s_rec_samples = 0;
    s_rec_stop = false;
    s_capture_done = false;
    s_rec_end = REC_END_USER;
    s_rec_end_pending = REC_END_NONE;
    s_rec_state = REC_RECORDING;
    xTaskNotifyGive(s_writer_task);
    xTaskNotifyGive(s_capture_task);
    ESP_LOGI(TAG, "Recording %s", s_rec_path);
    return ESP_OK;
}

void rec_record_set_paused(bool paused)
{
    if (s_rec_state == REC_RECORDING || s_rec_state == REC_PAUSED) {
        s_rec_state = paused ? REC_PAUSED : REC_RECORDING;
    }
}

void rec_record_stop(void)
{
    if (s_rec_state == REC_RECORDING || s_rec_state == REC_PAUSED) {
        s_rec_state = REC_SAVING;
        s_rec_stop = true;
    }
}

rec_state_t rec_record_state(void)
{
    return s_rec_state;
}

uint32_t rec_record_elapsed_ms(void)
{
    return (uint32_t)((uint64_t)s_rec_samples * 1000 / REC_SAMPLE_RATE);
}

int rec_record_level(void)
{
    return s_level;
}

rec_end_t rec_record_take_end(void)
{
    const rec_end_t end = s_rec_end_pending;
    s_rec_end_pending = REC_END_NONE;
    return end;
}

// ---------- playback ----------

static bool speaker_open(const rec_wav_info_t *info)
{
    if (s_speaker == NULL) {
        s_speaker = bsp_audio_codec_speaker_init();
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = info->sample_rate,
        .channel = info->channels,
        .bits_per_sample = 16,
    };
    if (s_speaker == NULL || esp_codec_dev_open(s_speaker, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Speaker unavailable");
        return false;
    }
    esp_codec_dev_set_out_vol(s_speaker, s_volume);
    s_volume_dirty = false;
    return true;
}

static void play_task(void *arg)
{
    static uint8_t chunk[PLAY_CHUNK];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        FILE *f = fopen(s_play_path, "rb");
        rec_wav_info_t info;
        if (f == NULL || rec_wav_read_info(f, &info) != ESP_OK) {
            ESP_LOGE(TAG, "Cannot play %s", s_play_path);
            if (f != NULL) {
                fclose(f);
            }
            s_play_state = REC_PLAY_FAILED;
            s_play_active = false;
            continue;
        }
        const uint32_t bytes_per_s = info.sample_rate * info.channels * 2;
        const uint32_t frame = info.channels * 2;
        s_play_dur_ms = (uint32_t)((uint64_t)info.data_bytes * 1000 / bytes_per_s);
        uint32_t pos = 0; // bytes into the data chunk
        fseek(f, info.data_offset, SEEK_SET);
        bool open = false;
        while (!s_play_stop) {
            if (s_play_toggle) {
                s_play_toggle = false;
                if (s_play_state == REC_PLAY_PLAYING) {
                    s_play_state = REC_PLAY_PAUSED;
                } else {
                    if (s_play_state == REC_PLAY_ENDED) {
                        pos = 0;
                        fseek(f, info.data_offset, SEEK_SET);
                    }
                    s_play_state = REC_PLAY_PLAYING;
                }
            }
            if (s_play_seek_ms != 0) {
                int64_t target = (int64_t)pos + (int64_t)s_play_seek_ms * bytes_per_s / 1000;
                s_play_seek_ms = 0;
                target = target < 0 ? 0 : target > info.data_bytes ? info.data_bytes : target;
                pos = (uint32_t)target / frame * frame;
                fseek(f, info.data_offset + pos, SEEK_SET);
                if (s_play_state == REC_PLAY_ENDED && pos < info.data_bytes) {
                    s_play_state = REC_PLAY_PAUSED;
                }
            }
            s_play_pos_ms = (uint32_t)((uint64_t)pos * 1000 / bytes_per_s);
            if (s_play_state != REC_PLAY_PLAYING) {
                if (open) { // the amp is off while paused
                    esp_codec_dev_close(s_speaker);
                    open = false;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (!open && !(open = speaker_open(&info))) {
                s_play_state = REC_PLAY_FAILED;
                break;
            }
            if (s_volume_dirty) {
                s_volume_dirty = false;
                esp_codec_dev_set_out_vol(s_speaker, s_volume);
            }
            size_t want = info.data_bytes - pos < sizeof(chunk) ? info.data_bytes - pos : sizeof(chunk);
            want -= want % frame;
            const size_t n = want > 0 ? fread(chunk, 1, want, f) : 0;
            if (n == 0) {
                s_play_state = REC_PLAY_ENDED;
                s_play_pos_ms = s_play_dur_ms;
                continue;
            }
            esp_codec_dev_write(s_speaker, chunk, (int)n);
            pos += n;
        }
        if (open) {
            esp_codec_dev_close(s_speaker);
        }
        fclose(f);
        if (s_play_state != REC_PLAY_FAILED) {
            s_play_state = REC_PLAY_STOPPED;
        }
        s_play_active = false;
    }
}

esp_err_t rec_play_start(const char *name)
{
    if (s_rec_state != REC_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    rec_play_stop();
    rec_files_path(name, s_play_path, sizeof(s_play_path));
    s_play_stop = false;
    s_play_toggle = false;
    s_play_seek_ms = 0;
    s_play_pos_ms = 0;
    s_play_dur_ms = 0;
    s_play_state = REC_PLAY_PLAYING;
    s_play_active = true;
    xTaskNotifyGive(s_play_task);
    return ESP_OK;
}

void rec_play_toggle(void)
{
    if (s_play_active) {
        s_play_toggle = true;
    }
}

void rec_play_seek(int32_t delta_ms)
{
    if (s_play_active) {
        s_play_seek_ms += delta_ms;
    }
}

void rec_play_stop(void)
{
    if (!s_play_active) {
        return;
    }
    s_play_stop = true;
    for (int i = 0; i < 100 && s_play_active; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

rec_play_state_t rec_play_state(void)
{
    return s_play_state;
}

uint32_t rec_play_position_ms(void)
{
    return s_play_pos_ms;
}

uint32_t rec_play_duration_ms(void)
{
    return s_play_dur_ms;
}

void rec_play_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s_volume_dirty = true;
}

// ---------- common ----------

bool rec_audio_busy(void)
{
    return s_rec_state != REC_IDLE || s_play_state == REC_PLAY_PLAYING;
}

esp_err_t rec_audio_init(void)
{
    uint8_t *storage = heap_caps_malloc(RING_BYTES + 1, MALLOC_CAP_SPIRAM);
    StaticStreamBuffer_t *ring = heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL);
    if (storage == NULL || ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Trigger level: the writer wakes with a full SD block (or on its 100 ms timeout).
    s_ring = xStreamBufferCreateStatic(RING_BYTES, WRITE_BYTES, storage, ring);
    if (xTaskCreate(capture_task, "rec_capture", 4096, NULL, 6, &s_capture_task) != pdPASS ||
        xTaskCreate(writer_task, "rec_writer", 4096, NULL, 5, &s_writer_task) != pdPASS ||
        xTaskCreate(play_task, "rec_play", 4096, NULL, 5, &s_play_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
