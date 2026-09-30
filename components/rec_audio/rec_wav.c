#include "rec_wav.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rec_wav";

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | (uint32_t)get16(p + 2) << 16;
}

void rec_wav_header(uint8_t *out, uint32_t sample_rate, uint16_t channels, uint32_t data_bytes)
{
    const uint16_t block = channels * 2;
    memcpy(out, "RIFF", 4);
    put32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16);
    put16(out + 20, 1); // PCM
    put16(out + 22, channels);
    put32(out + 24, sample_rate);
    put32(out + 28, sample_rate * block);
    put16(out + 32, block);
    put16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    put32(out + 40, data_bytes);
}

esp_err_t rec_wav_read_info(FILE *f, rec_wav_info_t *out)
{
    uint8_t h[12];
    if (fseek(f, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    const long size = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 ||
        memcmp(h + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    bool fmt = false;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t len = get32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0) {
            uint8_t b[16];
            if (len < 16 || fread(b, 1, 16, f) != 16) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (get16(b) != 1 || get16(b + 14) != 16 || get16(b + 2) == 0 || get16(b + 2) > 2) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            out->channels = get16(b + 2);
            out->sample_rate = get32(b + 4);
            out->bits = 16;
            fmt = true;
            if (fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR) != 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else if (memcmp(c, "data", 4) == 0) {
            if (!fmt) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            out->data_offset = (uint32_t)ftell(f);
            const uint32_t avail = size > (long)out->data_offset ? (uint32_t)(size - out->data_offset) : 0;
            out->data_bytes = len == 0 || len > avail ? avail : len;
            return ESP_OK;
        } else if (fseek(f, (long)(len + (len & 1)), SEEK_CUR) != 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
}

bool rec_wav_repair(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        return false;
    }
    bool changed = false;
    uint8_t h[REC_WAV_HEADER_BYTES];
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size >= REC_WAV_HEADER_BYTES && fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, "RIFF", 4) == 0 &&
        memcmp(h + 36, "data", 4) == 0 && get16(h + 20) == 1 && get16(h + 34) == 16) {
        const uint32_t data = (uint32_t)(size - REC_WAV_HEADER_BYTES);
        if (get32(h + 40) != data || get32(h + 4) != 36 + data) {
            rec_wav_header(h, get32(h + 24), get16(h + 22), data);
            fseek(f, 0, SEEK_SET);
            changed = fwrite(h, 1, sizeof(h), f) == sizeof(h);
            ESP_LOGW(TAG, "Repaired %s (%u bytes of audio)", path, (unsigned)data);
        }
    }
    fclose(f);
    return changed;
}
