#include "esp_err.h"
#include "esp_log.h"

#include "rec_app.h"
#include "watch_launcher.h"

static const char *TAG = "ESP32WatchRecorder";

void app_main(void)
{
    // Launcher mode: first thing, so any reset from here on returns to the launcher.
    watch_launcher_boot_once();

    esp_err_t err = rec_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start: %s", esp_err_to_name(err));
    }
}
