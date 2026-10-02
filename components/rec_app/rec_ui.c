#include "rec_ui.h"

#include <stdio.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rec_audio.h"
#include "watch_buttons.h"
#include "watch_display.h"
#include "watch_power.h"

#define STACK_DEPTH 6
#define MAX_FOCUS 64
#define LOOP_MS 20
#define PWR_POLL_MS 50
#define BOOT_LONG_MS 700
#define DIM_US 3000000 // dimmed screen before it turns off
#define DIM_PERCENT 15 // dimmed brightness, as a share of the set brightness
#define DIM_MIN 3

static const char *TAG = "rec_ui";

static struct {
    const rec_screen_t *stack[STACK_DEPTH];
    int depth;
    lv_obj_t *root;
    lv_obj_t *title_clock;
    lv_obj_t *focus[MAX_FOCUS];
    int focus_count;
    int focus_index;
    bool sleep_requested;
    int timeout_s;
    bool dimmed;
    int64_t dim_start;
    lv_obj_t *dim_shield; // transparent layer that swallows the touch that undims
    volatile bool dim_touched;
} s_ui;

// ---------- screen stack ----------

static void show_top(void)
{
    const rec_screen_t *screen = s_ui.stack[s_ui.depth - 1];
    lv_obj_t *old = s_ui.root;

    s_ui.focus_count = 0;
    s_ui.focus_index = -1;
    s_ui.title_clock = NULL;
    s_ui.root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_ui.root);
    lv_obj_set_size(s_ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.root, lv_color_hex(REC_BG), 0);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ui.root, lv_color_hex(REC_TEXT), 0);
    lv_obj_set_style_text_font(s_ui.root, &font_barlow_18, 0);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);
    screen->create(s_ui.root);
    lv_screen_load(s_ui.root);
    if (screen->tick != NULL) {
        screen->tick();
    }
    if (old != NULL) {
        // Deferred: a click handler inside the old screen may be running this.
        lv_obj_delete_async(old);
    }
    ESP_LOGD(TAG, "Screen %s", screen->name);
}

static void destroy_top(void)
{
    const rec_screen_t *screen = s_ui.stack[s_ui.depth - 1];
    if (screen->destroy != NULL) {
        screen->destroy();
    }
}

void rec_push(const rec_screen_t *screen)
{
    if (s_ui.depth == STACK_DEPTH) {
        return;
    }
    if (s_ui.depth > 0) {
        destroy_top();
    }
    s_ui.stack[s_ui.depth++] = screen;
    show_top();
}

void rec_pop(int count)
{
    if (s_ui.depth <= 1 || count <= 0) {
        return;
    }
    destroy_top();
    s_ui.depth = s_ui.depth - count < 1 ? 1 : s_ui.depth - count;
    show_top();
}

void rec_back(void)
{
    rec_pop(1);
}

void rec_request_sleep(void)
{
    s_ui.sleep_requested = true;
}

// ---------- focus ----------

void rec_focus_add(lv_obj_t *obj)
{
    if (s_ui.focus_count < MAX_FOCUS) {
        s_ui.focus[s_ui.focus_count++] = obj;
    }
}

void rec_focus_set(int index)
{
    if (s_ui.focus_count == 0) {
        return;
    }
    if (s_ui.focus_index >= 0) {
        lv_obj_remove_state(s_ui.focus[s_ui.focus_index], REC_FOCUS_STATE);
    }
    s_ui.focus_index = ((index % s_ui.focus_count) + s_ui.focus_count) % s_ui.focus_count;
    lv_obj_add_state(s_ui.focus[s_ui.focus_index], REC_FOCUS_STATE);
    lv_obj_scroll_to_view_recursive(s_ui.focus[s_ui.focus_index], LV_ANIM_ON);
}

void rec_focus_set_obj(lv_obj_t *obj)
{
    for (int i = 0; i < s_ui.focus_count; i++) {
        if (s_ui.focus[i] == obj) {
            rec_focus_set(i);
            return;
        }
    }
}

// ---------- input dispatch (LVGL lock held) ----------

static void on_boot_short(void)
{
    const rec_screen_t *screen = s_ui.stack[s_ui.depth - 1];
    if (screen->on_boot != NULL && screen->on_boot()) {
        return;
    }
    if (s_ui.focus_count > 0) {
        if (s_ui.focus_index < 0) {
            rec_focus_set(0);
        }
        lv_obj_t *target = s_ui.focus[s_ui.focus_index];
        if (!lv_obj_has_state(target, LV_STATE_DISABLED)) {
            lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
        }
    }
}

static void on_boot_long(void)
{
    rec_focus_set(s_ui.focus_index + 1);
}

static void on_pwr(void)
{
    const rec_screen_t *screen = s_ui.stack[s_ui.depth - 1];
    if (screen->on_pwr != NULL && screen->on_pwr()) {
        return;
    }
    if (s_ui.depth > 1) {
        rec_back();
    } else {
        s_ui.sleep_requested = true;
    }
}

static void title_clock_update(void);

// ---------- dimming (LVGL lock held) ----------

static void shield_pressed(lv_event_t *e)
{
    s_ui.dim_touched = true;
}

// Timeout reached: dim first, and put a clickable transparent layer on top so a touch
// during the dim only brings the screen back instead of reaching the widget under it.
static void dim_start(int64_t now)
{
    const int level = watch_display_get_brightness() * DIM_PERCENT / 100;
    bsp_display_brightness_set(level < DIM_MIN ? DIM_MIN : level); // keeps the saved level
    s_ui.dim_shield = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ui.dim_shield);
    lv_obj_set_size(s_ui.dim_shield, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_ui.dim_shield, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.dim_shield, shield_pressed, LV_EVENT_PRESSED, NULL);
    s_ui.dim_touched = false;
    s_ui.dim_start = now;
    s_ui.dimmed = true;
}

static void dim_end(bool restore)
{
    if (s_ui.dim_shield != NULL) {
        lv_obj_delete(s_ui.dim_shield);
        s_ui.dim_shield = NULL;
    }
    if (restore) {
        watch_display_set_brightness(watch_display_get_brightness());
        lv_display_trigger_activity(NULL);
    }
    s_ui.dimmed = false;
}

// ---------- system task ----------

static void system_task(void *arg)
{
    watch_boot_debouncer_t boot;
    watch_boot_debouncer_init(&boot, 0, BOOT_LONG_MS);
    int64_t last_pwr = 0;
    int64_t last_tick = 0;
    bool swallow_boot = false; // the BOOT press that undimmed: ignore it until released
    for (;;) {
        const int64_t now = esp_timer_get_time();
        const watch_boot_event_t ev = watch_boot_debouncer_poll(&boot);
        bool pwr = false;
        if (now - last_pwr >= PWR_POLL_MS * 1000) {
            last_pwr = now;
            watch_pwr_key_take_short_press(&pwr);
        }

        bsp_display_lock(0);
        if (s_ui.dimmed && (ev.down || pwr || s_ui.dim_touched)) {
            dim_end(true);
            swallow_boot = ev.down || ev.held;
            pwr = false;
        }
        if (swallow_boot) {
            swallow_boot = ev.held;
        } else {
            if (ev.down || pwr) {
                lv_display_trigger_activity(NULL);
            }
            if (ev.short_press) {
                on_boot_short();
            }
            if (ev.long_press) {
                on_boot_long();
            }
            if (pwr) {
                on_pwr();
            }
        }
        const rec_screen_t *screen = s_ui.stack[s_ui.depth - 1];
        if (now - last_tick >= 1000000) {
            last_tick = now;
            if (screen->tick != NULL) {
                screen->tick();
            }
            title_clock_update();
        }
        const uint32_t idle_ms = lv_display_get_inactive_time(NULL);
        if (s_ui.dimmed) {
            if (now - s_ui.dim_start >= DIM_US) {
                s_ui.sleep_requested = true;
            }
        } else if (!screen->keep_awake && idle_ms > (uint32_t)s_ui.timeout_s * 1000) {
            dim_start(now);
        }
        if (s_ui.sleep_requested && s_ui.dimmed) {
            dim_end(false); // the screen-off call restores the saved brightness on wake
        }
        const bool sleep = s_ui.sleep_requested;
        s_ui.sleep_requested = false;
        bsp_display_unlock();

        if (sleep) {
            // Recording or playing keeps the chip awake with the screen off; when that
            // ends it light-sleeps until BOOT or PWR, like the launcher.
            const watch_wake_t why = watch_power_screen_off(0, rec_audio_busy);
            ESP_LOGI(TAG, "Woke up by %s", why == WATCH_WAKE_BOOT ? "BOOT" : "PWR");
            watch_boot_debouncer_init(&boot, 0, BOOT_LONG_MS);
            bsp_display_lock(0);
            screen = s_ui.stack[s_ui.depth - 1];
            if (screen->tick != NULL) {
                screen->tick();
            }
            title_clock_update();
            bsp_display_unlock();
            last_tick = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}

esp_err_t rec_ui_start(const rec_screen_t *root, int screen_timeout_s)
{
    s_ui.depth = 0;
    s_ui.timeout_s = screen_timeout_s;
    rec_push(root);
    if (xTaskCreate(system_task, "rec_system", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "System task not created: no buttons, timeout or sleep");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------- widgets ----------

lv_obj_t *rec_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text != NULL ? text : "");
    return label;
}

static void title_clock_update(void)
{
    if (s_ui.title_clock != NULL) {
        const time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        lv_label_set_text_fmt(s_ui.title_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
}

lv_obj_t *rec_row(lv_obj_t *parent, int gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *rec_title(lv_obj_t *root, const char *title)
{
    lv_obj_t *row = rec_row(root, 12);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 30);
    rec_label(row, &font_title_30, REC_TEXT, title);
    lv_obj_t *clock = rec_label(row, &font_barlow_18, REC_MUTED, "");
    lv_obj_set_style_pad_bottom(clock, 4, 0);
    s_ui.title_clock = clock; // updated every second by the system task
    title_clock_update();
    return row;
}

lv_obj_t *rec_hint(lv_obj_t *root, const char *text)
{
    lv_obj_t *hint = rec_label(root, &font_barlow_16, REC_MUTED, text);
    lv_obj_set_style_text_letter_space(hint, 1, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -28);
    return hint;
}

void rec_style_focus_ring(lv_obj_t *obj, int width)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(REC_FOCUS), REC_FOCUS_STATE);
    lv_obj_set_style_border_width(obj, width, REC_FOCUS_STATE);
    lv_obj_set_style_border_color(obj, lv_color_hex(REC_FOCUS), LV_STATE_PRESSED);
}

lv_obj_t *rec_round_button(lv_obj_t *parent, int diameter, const char *icon, const lv_font_t *font,
                           lv_event_cb_t on_click, void *user)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, diameter, diameter);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(REC_SURFACE), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1F1F1F), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(REC_BORDER), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    rec_style_focus_ring(button, 3);
    if (icon != NULL) {
        lv_obj_t *glyph = rec_label(button, font, REC_TEXT, icon);
        lv_obj_center(glyph);
    }
    if (on_click != NULL) {
        lv_obj_add_event_cb(button, on_click, LV_EVENT_CLICKED, user);
    }
    return button;
}

lv_obj_t *rec_pill_button(lv_obj_t *parent, const char *text, uint32_t fill, lv_event_cb_t on_click,
                          void *user)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 150, 60);
    lv_obj_set_style_radius(button, 30, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(REC_BORDER), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_opa(button, LV_OPA_70, LV_STATE_PRESSED);
    rec_style_focus_ring(button, 3);
    lv_obj_center(rec_label(button, &font_barlow_semibold_22, REC_TEXT, text));
    if (on_click != NULL) {
        lv_obj_add_event_cb(button, on_click, LV_EVENT_CLICKED, user);
    }
    return button;
}

void rec_format_duration(char *out, size_t size, uint32_t ms)
{
    const uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(out, size, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    } else {
        snprintf(out, size, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    }
}

void rec_format_bytes(char *out, size_t size, uint64_t bytes)
{
    if (bytes >= 1000ULL * 1024 * 1024) {
        const unsigned tenths = (unsigned)(bytes * 10 / (1024ULL * 1024 * 1024));
        snprintf(out, size, "%u,%u GB", tenths / 10, tenths % 10);
    } else if (bytes >= 1024ULL * 1024) {
        const unsigned tenths = (unsigned)(bytes * 10 / (1024ULL * 1024));
        if (tenths >= 1000) {
            snprintf(out, size, "%u MB", tenths / 10);
        } else {
            snprintf(out, size, "%u,%u MB", tenths / 10, tenths % 10);
        }
    } else {
        snprintf(out, size, "%u KB", (unsigned)((bytes + 1023) / 1024));
    }
}
