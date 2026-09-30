// Root screen: time, live level meter, record/stop, pause and the recordings button.
#include <stdio.h>

#include "rec_audio.h"
#include "rec_screens.h"
#include "watch_launcher.h"

#define METER_BARS 33
#define METER_H 72
#define REFRESH_MS 50
#define MESSAGE_MS 4000

static struct {
    lv_obj_t *status_dot;
    lv_obj_t *status;
    lv_obj_t *time;
    lv_obj_t *bars[METER_BARS];
    lv_obj_t *record;
    lv_obj_t *core; // red circle (idle) or square (recording) inside the record button
    lv_obj_t *pause;
    lv_obj_t *list;
    lv_obj_t *count;
    lv_obj_t *hint;
    lv_timer_t *timer;
    int levels[METER_BARS];
    uint32_t message_until;
    rec_state_t shown_state;
    bool shown_mounted;
} s;

static void show_message(const char *text, uint32_t color)
{
    lv_label_set_text(s.hint, text);
    lv_obj_set_style_text_color(s.hint, lv_color_hex(color), 0);
    s.message_until = lv_tick_get() + MESSAGE_MS;
}

static void refresh_space_and_count(void)
{
    int total = 0;
    rec_files_list(NULL, 0, &total);
    lv_label_set_text_fmt(s.count, "%d", total);
    lv_obj_set_style_opa(s.count, total > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (lv_tick_get() < s.message_until) {
        return;
    }
    char text[48];
    uint64_t free_bytes = 0, total_bytes = 0;
    if (rec_files_space(&free_bytes, &total_bytes)) {
        char size[16];
        rec_format_bytes(size, sizeof(size), free_bytes);
        const unsigned hours = (unsigned)(free_bytes / REC_BYTES_PER_S / 3600);
        snprintf(text, sizeof(text), "Libre %s · ~%u h", size, hours);
    } else {
        snprintf(text, sizeof(text), "Sin microSD");
    }
    lv_label_set_text(s.hint, text);
    lv_obj_set_style_text_color(s.hint, lv_color_hex(REC_MUTED), 0);
}

static void set_core_shape(bool square)
{
    const int size = square ? 34 : 44;
    lv_obj_set_size(s.core, size, size);
    lv_obj_set_style_radius(s.core, square ? 8 : LV_RADIUS_CIRCLE, 0);
    lv_obj_center(s.core);
}

static void set_enabled(lv_obj_t *obj, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

// State-dependent labels and buttons; runs every refresh, changes only on transitions.
static void refresh_state(void)
{
    const rec_state_t state = rec_record_state();
    const bool mounted = rec_files_mounted();
    if (state == s.shown_state && mounted == s.shown_mounted) {
        return;
    }
    const bool was_active = s.shown_state != REC_IDLE;
    s.shown_state = state;
    s.shown_mounted = mounted;
    const bool active = state == REC_RECORDING || state == REC_PAUSED;
    set_core_shape(active);
    set_enabled(s.record, state != REC_SAVING);
    set_enabled(s.pause, active);
    set_enabled(s.list, state == REC_IDLE && mounted);
    lv_label_set_text(lv_obj_get_child(s.pause, 0), state == REC_PAUSED ? REC_ICON_PLAY : REC_ICON_PAUSE);
    lv_obj_set_style_opa(s.status_dot, state == REC_IDLE ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    uint32_t color = REC_MUTED;
    const char *text = mounted ? "Listo para grabar" : "Inserta una microSD";
    switch (state) {
    case REC_RECORDING:
        text = "Grabando";
        color = REC_ACCENT;
        break;
    case REC_PAUSED:
        text = "En pausa";
        color = REC_WARN;
        break;
    case REC_SAVING:
        text = "Guardando...";
        color = REC_TEXT_2;
        break;
    default:
        color = mounted ? REC_MUTED : REC_WARN;
        break;
    }
    lv_label_set_text(s.status, text);
    lv_obj_set_style_text_color(s.status, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s.status_dot, lv_color_hex(color), 0);

    if (state == REC_IDLE && was_active) {
        switch (rec_record_take_end()) {
        case REC_END_USER:
            show_message("Grabación guardada", REC_TEXT_2);
            break;
        case REC_END_FULL:
            show_message("SD llena: grabación guardada", REC_WARN);
            break;
        case REC_END_ERROR:
            show_message("Error de la SD o del micro", REC_WARN);
            break;
        case REC_END_SHORT:
            show_message("Demasiado corta: descartada", REC_MUTED);
            break;
        default:
            break;
        }
        refresh_space_and_count();
    }
}

static void refresh_cb(lv_timer_t *t)
{
    const rec_state_t state = rec_record_state();
    refresh_state();
    if (s.message_until != 0 && lv_tick_get() >= s.message_until) {
        s.message_until = 0;
        refresh_space_and_count();
    }

    // Time
    char text[16];
    const uint32_t ms = state == REC_IDLE ? 0 : rec_record_elapsed_ms();
    const uint32_t sec = ms / 1000;
    if (sec >= 3600) {
        snprintf(text, sizeof(text), "%u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60),
                 (unsigned)(sec % 60));
    } else {
        snprintf(text, sizeof(text), "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    }
    lv_label_set_text(s.time, text);

    // Recording dot blinks once a second.
    if (state == REC_RECORDING) {
        lv_obj_set_style_opa(s.status_dot, (lv_tick_get() / 500) % 2 ? LV_OPA_30 : LV_OPA_COVER, 0);
    }

    // Meter: scrolls right to left, one bar per refresh.
    for (int i = 0; i < METER_BARS - 1; i++) {
        s.levels[i] = s.levels[i + 1];
    }
    s.levels[METER_BARS - 1] = state == REC_RECORDING || state == REC_PAUSED ? rec_record_level() : 0;
    const uint32_t bar_color = state == REC_RECORDING ? REC_ACCENT : state == REC_PAUSED ? REC_MUTED : REC_TRACK;
    for (int i = 0; i < METER_BARS; i++) {
        const int h = 6 + s.levels[i] * (METER_H - 6) / 100;
        lv_obj_set_height(s.bars[i], h);
        lv_obj_set_style_bg_color(s.bars[i], lv_color_hex(bar_color), 0);
    }
}

static void record_clicked(lv_event_t *e)
{
    const rec_state_t state = rec_record_state();
    if (state == REC_RECORDING || state == REC_PAUSED) {
        rec_record_stop();
        return;
    }
    if (state != REC_IDLE) {
        return;
    }
    switch (rec_record_start()) {
    case ESP_OK:
        s.message_until = 0;
        break;
    case ESP_ERR_INVALID_STATE:
        show_message("Sin microSD", REC_WARN);
        break;
    case ESP_ERR_NO_MEM:
        show_message("SD llena", REC_WARN);
        break;
    default:
        show_message("No se pudo empezar a grabar", REC_WARN);
        break;
    }
    s.shown_mounted = !rec_files_mounted(); // force a state refresh
    refresh_state();
    refresh_space_and_count();
}

static void pause_clicked(lv_event_t *e)
{
    rec_record_set_paused(rec_record_state() == REC_RECORDING);
}

static void list_clicked(lv_event_t *e)
{
    rec_push(&rec_list_screen);
}

static void record_create(lv_obj_t *root)
{
    rec_title(root, "Grabadora");

    lv_obj_t *status_row = rec_row(root, 10);
    lv_obj_align(status_row, LV_ALIGN_TOP_MID, 0, 88);
    s.status_dot = lv_obj_create(status_row);
    lv_obj_remove_style_all(s.status_dot);
    lv_obj_set_size(s.status_dot, 12, 12);
    lv_obj_set_style_radius(s.status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s.status_dot, LV_OPA_COVER, 0);
    s.status = rec_label(status_row, &font_barlow_20, REC_MUTED, "");

    s.time = rec_label(root, &font_digits_72, REC_TEXT, "00:00");
    lv_obj_align(s.time, LV_ALIGN_TOP_MID, 0, 116);

    lv_obj_t *meter = rec_row(root, 4);
    lv_obj_set_height(meter, METER_H);
    lv_obj_align(meter, LV_ALIGN_TOP_MID, 0, 214);
    for (int i = 0; i < METER_BARS; i++) {
        lv_obj_t *bar = lv_obj_create(meter);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 6, 6);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(REC_TRACK), 0);
        s.bars[i] = bar;
        s.levels[i] = 0;
    }

    lv_obj_t *buttons = rec_row(root, 34);
    lv_obj_align(buttons, LV_ALIGN_TOP_MID, 0, 314);
    s.list = rec_round_button(buttons, 64, REC_ICON_LIST, &font_icons_24, list_clicked, NULL);
    s.count = rec_label(s.list, &font_barlow_16, REC_TEXT, "");
    lv_obj_set_style_bg_color(s.count, lv_color_hex(REC_ACCENT), 0);
    lv_obj_set_style_bg_opa(s.count, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s.count, 10, 0);
    lv_obj_set_style_pad_hor(s.count, 6, 0);
    lv_obj_set_style_min_width(s.count, 20, 0);
    lv_obj_set_style_text_align(s.count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s.count, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(s.count, LV_ALIGN_TOP_RIGHT, 6, -4);

    s.record = rec_round_button(buttons, 104, NULL, NULL, record_clicked, NULL);
    lv_obj_set_style_bg_color(s.record, lv_color_hex(REC_ACCENT_DIM), 0);
    lv_obj_set_style_border_color(s.record, lv_color_hex(REC_ACCENT), 0);
    lv_obj_set_style_border_width(s.record, 2, 0);
    lv_obj_set_style_border_width(s.record, 4, REC_FOCUS_STATE);
    s.core = lv_obj_create(s.record);
    lv_obj_remove_style_all(s.core);
    lv_obj_set_style_bg_color(s.core, lv_color_hex(REC_ACCENT), 0);
    lv_obj_set_style_bg_opa(s.core, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s.core, LV_OBJ_FLAG_CLICKABLE);

    s.pause = rec_round_button(buttons, 64, REC_ICON_PAUSE, &font_icons_24, pause_clicked, NULL);

    s.hint = rec_hint(root, "");
    rec_focus_add(s.list);
    rec_focus_add(s.record);
    rec_focus_add(s.pause);
    rec_focus_set_obj(s.record);

    s.shown_state = (rec_state_t)-1;
    s.message_until = 0;
    refresh_state();
    refresh_space_and_count();
    s.timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    refresh_cb(s.timer);
}

static void record_destroy(void)
{
    if (s.timer != NULL) {
        lv_timer_delete(s.timer);
        s.timer = NULL;
    }
}

// PWR: while recording, turn the screen off and keep going; otherwise leave the app.
static bool record_pwr(void)
{
    if (rec_record_state() == REC_IDLE && watch_launcher_is_available()) {
        watch_launcher_exit();
    }
    rec_request_sleep();
    return true;
}

const rec_screen_t rec_record_screen = {
    .name = "record",
    .create = record_create,
    .destroy = record_destroy,
    .on_pwr = record_pwr,
};
