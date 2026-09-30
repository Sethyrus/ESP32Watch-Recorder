#include "rec_files.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "rec_audio.h"
#include "rec_wav.h"

static const char *TAG = "rec_files";
static bool s_mounted;

static bool is_wav(const char *name)
{
    const size_t n = strlen(name);
    return n > 4 && n < REC_NAME_MAX && strcasecmp(name + n - 4, ".wav") == 0 && name[0] != '.';
}

void rec_files_path(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", REC_DIR, name);
}

static void repair_all(void)
{
    DIR *dir = opendir(REC_DIR);
    if (dir == NULL) {
        return;
    }
    struct dirent *e;
    char path[64];
    while ((e = readdir(dir)) != NULL) {
        if (is_wav(e->d_name)) {
            rec_files_path(e->d_name, path, sizeof(path));
            rec_wav_repair(path);
        }
    }
    closedir(dir);
}

esp_err_t rec_files_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No microSD: %s", esp_err_to_name(err));
        return err;
    }
    if (mkdir(REC_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Cannot create %s: errno %d", REC_DIR, errno);
        bsp_sdcard_unmount();
        return ESP_FAIL;
    }
    s_mounted = true;
    repair_all();
    uint64_t free_bytes = 0, total = 0;
    rec_files_space(&free_bytes, &total);
    ESP_LOGI(TAG, "microSD mounted: %llu MB free of %llu MB", free_bytes >> 20, total >> 20);
    return ESP_OK;
}

bool rec_files_mounted(void)
{
    return s_mounted;
}

static int newest_first(const void *a, const void *b)
{
    return strcmp(((const rec_file_t *)b)->name, ((const rec_file_t *)a)->name);
}

int rec_files_list(rec_file_t *out, int max, int *total)
{
    int count = 0, seen = 0;
    DIR *dir = s_mounted ? opendir(REC_DIR) : NULL;
    if (dir != NULL) {
        struct dirent *e;
        char path[64];
        while ((e = readdir(dir)) != NULL) {
            if (!is_wav(e->d_name)) {
                continue;
            }
            seen++;
            rec_files_path(e->d_name, path, sizeof(path));
            struct stat st;
            if (stat(path, &st) != 0) {
                continue;
            }
            rec_file_t file;
            strlcpy(file.name, e->d_name, sizeof(file.name));
            file.bytes = (uint32_t)st.st_size;
            const uint32_t data = file.bytes > 44 ? file.bytes - 44 : 0;
            file.duration_ms = (uint32_t)((uint64_t)data * 1000 / REC_BYTES_PER_S);
            if (count < max) {
                out[count++] = file;
            } else if (max > 0) {
                // Keep the newest max: replace the oldest kept if this one is newer.
                int oldest = 0;
                for (int i = 1; i < count; i++) {
                    if (strcmp(out[i].name, out[oldest].name) < 0) {
                        oldest = i;
                    }
                }
                if (strcmp(file.name, out[oldest].name) > 0) {
                    out[oldest] = file;
                }
            }
        }
        closedir(dir);
    }
    qsort(out, count, sizeof(out[0]), newest_first);
    if (total != NULL) {
        *total = seen;
    }
    return count;
}

esp_err_t rec_files_delete(const char *name)
{
    char path[64];
    rec_files_path(name, path, sizeof(path));
    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "Cannot delete %s: errno %d", path, errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Deleted %s", name);
    return ESP_OK;
}

bool rec_files_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
    if (!s_mounted) {
        return false;
    }
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_b) != ESP_OK) {
        return false;
    }
    *free_bytes = free_b;
    *total_bytes = total;
    return true;
}

void rec_files_title(const char *name, char *out, size_t size)
{
    static const char *const months[] = {"ene", "feb", "mar", "abr", "may", "jun",
                                         "jul", "ago", "sep", "oct", "nov", "dic"};
    int y, mo, d, h, mi, s;
    if (sscanf(name, "%4d%2d%2d_%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6 && mo >= 1 && mo <= 12) {
        snprintf(out, size, "%d %s · %02d:%02d", d, months[mo - 1], h, mi);
    } else {
        snprintf(out, size, "%s", name);
    }
}
