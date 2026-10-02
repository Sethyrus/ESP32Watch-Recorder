#include "rec_app.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "nvs.h"
#include "rec_audio.h"
#include "rec_screens.h"
#include "watch_buttons.h"
#include "watch_display.h"
#include "watch_nvs.h"
#include "watch_power.h"
#include "watch_rtc.h"

#define NS "recorder"
#define LAUNCHER_NS "launcher" // read only: brightness and screen timeout set there

static const char *TAG = "rec_app";
static const uint8_t VOLUMES[REC_VOLUME_LEVELS] = {50, 65, 80, 90, 100};
static int s_volume_level = 3;

int rec_volume_level(void)
{
    return s_volume_level;
}

void rec_volume_step(void)
{
    s_volume_level = s_volume_level % REC_VOLUME_LEVELS + 1;
    rec_play_set_volume(VOLUMES[s_volume_level - 1]);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol", (uint8_t)s_volume_level);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Brightness and screen timeout follow the launcher's settings, when it has saved any.
static void load_settings(int *brightness, int *timeout_s)
{
    nvs_handle_t h;
    uint8_t u8;
    uint16_t u16;
    if (nvs_open(LAUNCHER_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "bright", &u8) == ESP_OK && u8 >= 10 && u8 <= 100) {
            *brightness = u8;
        }
        if (nvs_get_u16(h, "timeout", &u16) == ESP_OK && u16 >= 5 && u16 <= 600) {
            *timeout_s = u16;
        }
        nvs_close(h);
    }
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "vol", &u8) == ESP_OK && u8 >= 1 && u8 <= REC_VOLUME_LEVELS) {
            s_volume_level = u8;
        }
        nvs_close(h);
    }
}

esp_err_t rec_app_start(void)
{
    esp_err_t err = watch_nvs_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable, settings not kept: %s", esp_err_to_name(err));
    }
    int brightness = 80, timeout_s = 15;
    load_settings(&brightness, &timeout_s);

    // The IMU may still be on from another app (esp_restart() does not reset it).
    watch_power_quiet_peripherals();

    watch_display_config_t display = WATCH_DISPLAY_CONFIG_DEFAULT();
    display.brightness = brightness;
    if (watch_display_start(&display) == NULL) {
        return ESP_FAIL;
    }
    if ((err = watch_boot_button_init()) != ESP_OK) {
        ESP_LOGW(TAG, "BOOT button unavailable: %s", esp_err_to_name(err));
    }
    if ((err = watch_pwr_key_init()) != ESP_OK) {
        ESP_LOGW(TAG, "PWR key unavailable: %s", esp_err_to_name(err));
    }
    if ((err = watch_rtc_init(false)) != ESP_OK) { // file names come from the clock
        ESP_LOGW(TAG, "RTC unavailable: %s", esp_err_to_name(err));
    }
    if ((err = rec_audio_init()) != ESP_OK) {
        return err;
    }
    rec_play_set_volume(VOLUMES[s_volume_level - 1]);
    rec_files_mount(); // no card is not fatal: the UI says so and retries on record

    if (!bsp_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    err = rec_ui_start(&rec_record_screen, timeout_s);
    bsp_display_unlock();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "Recorder ready");
    return ESP_OK;
}
