#pragma once

#include "esp_err.h"

// Starts the recorder: board services, microSD, audio engine and the UI.
esp_err_t rec_app_start(void);
