// Player (play/pause, seek 10 s, volume, delete) and the delete confirmation.
#include <stdio.h>

#include "rec_audio.h"
#include "rec_screens.h"

#define REFRESH_MS 100
#define SEEK_MS 10000

static rec_file_t s_file;
static bool s_autoplay;

static struct {
    lv_obj_t *position;
    lv_obj_t *bar;
    lv_obj_t *remaining;
    lv_obj_t *total;
    lv_obj_t *play;
    lv_obj_t *volume_bars[REC_VOLUME_LEVELS];
    lv_obj_t *status;
    lv_timer_t *timer;
} s;

void rec_player_open(const rec_file_t *file)
{
    s_file = *file;
    s_autoplay = true;
    rec_push(&rec_player_screen);
}

static void volume_refresh(void)
{
    for (int i = 0; i < REC_VOLUME_LEVELS; i++) {
        lv_obj_set_style_bg_color(s.volume_bars[i],
                                  lv_color_hex(i < rec_volume_level() ? REC_TEXT : REC_TRACK), 0);
    }
}

static void refresh_cb(lv_timer_t *t)
{
    const rec_play_state_t state = rec_play_state();
    // Before the engine knows the real duration, use the one from the file size.
    uint32_t duration = rec_play_duration_ms();
    if (duration == 0) {
        duration = s_file.duration_ms;
    }
    const bool started = state != REC_PLAY_STOPPED && state != REC_PLAY_FAILED;
    const uint32_t position = started ? rec_play_position_ms() : 0;
    char text[16];
    rec_format_duration(text, sizeof(text), position);
    lv_label_set_text(s.position, text);
    rec_format_duration(text, sizeof(text), duration - (position < duration ? position : duration));
    lv_label_set_text_fmt(s.remaining, "-%s", text);
    rec_format_duration(text, sizeof(text), duration);
    lv_label_set_text(s.total, text);
    lv_bar_set_value(s.bar, duration > 0 ? (int32_t)((uint64_t)position * 1000 / duration) : 0, LV_ANIM_OFF);
    lv_label_set_text(lv_obj_get_child(s.play, 0), state == REC_PLAY_PLAYING ? REC_ICON_PAUSE : REC_ICON_PLAY);
    if (state == REC_PLAY_FAILED) {
        lv_label_set_text(s.status, "No se puede reproducir");
        lv_obj_set_style_text_color(s.status, lv_color_hex(REC_WARN), 0);
    }
}

static void play_clicked(lv_event_t *e)
{
    const rec_play_state_t state = rec_play_state();
    if (state == REC_PLAY_STOPPED || state == REC_PLAY_FAILED) {
        rec_play_start(s_file.name);
    } else {
        rec_play_toggle();
    }
    refresh_cb(s.timer);
}

static void seek_clicked(lv_event_t *e)
{
    rec_play_seek((int32_t)(intptr_t)lv_event_get_user_data(e));
}

static void volume_clicked(lv_event_t *e)
{
    rec_volume_step();
    volume_refresh();
}

static void delete_clicked(lv_event_t *e)
{
    rec_push(&rec_delete_screen);
}

static void player_create(lv_obj_t *root)
{
    char title[40], size[16];
    rec_files_title(s_file.name, title, sizeof(title));
    lv_obj_t *head = rec_label(root, &font_title_30, REC_TEXT, title);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 30);
    rec_format_bytes(size, sizeof(size), s_file.bytes);
    s.status = rec_label(root, &font_barlow_16, REC_MUTED, size);
    lv_obj_align(s.status, LV_ALIGN_TOP_MID, 0, 70);

    s.position = rec_label(root, &font_digits_72, REC_TEXT, "0:00");
    lv_obj_align(s.position, LV_ALIGN_TOP_MID, 0, 104);

    s.bar = lv_bar_create(root);
    lv_obj_set_size(s.bar, 320, 8);
    lv_obj_align(s.bar, LV_ALIGN_TOP_MID, 0, 206);
    lv_bar_set_range(s.bar, 0, 1000);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(REC_TRACK), 0);
    lv_obj_set_style_bg_opa(s.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s.bar, 4, 0);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(REC_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s.bar, 4, LV_PART_INDICATOR);
    s.remaining = rec_label(root, &font_barlow_16, REC_MUTED, "");
    lv_obj_align(s.remaining, LV_ALIGN_TOP_LEFT, 45, 222); // under the bar's ends
    s.total = rec_label(root, &font_barlow_16, REC_MUTED, "");
    lv_obj_align(s.total, LV_ALIGN_TOP_RIGHT, -45, 222);

    lv_obj_t *transport = rec_row(root, 30);
    lv_obj_align(transport, LV_ALIGN_TOP_MID, 0, 262);
    lv_obj_t *back = rec_round_button(transport, 64, REC_ICON_BACK10, &font_icons_24, seek_clicked,
                                      (void *)(intptr_t)-SEEK_MS);
    s.play = rec_round_button(transport, 88, REC_ICON_PLAY, &font_icons_32, play_clicked, NULL);
    lv_obj_set_style_bg_color(s.play, lv_color_hex(REC_ACCENT_DIM), 0);
    lv_obj_set_style_border_color(s.play, lv_color_hex(REC_ACCENT), 0);
    lv_obj_t *fwd = rec_round_button(transport, 64, REC_ICON_FWD10, &font_icons_24, seek_clicked,
                                     (void *)(intptr_t)SEEK_MS);

    lv_obj_t *extras = rec_row(root, 14);
    lv_obj_align(extras, LV_ALIGN_TOP_MID, 0, 376);
    lv_obj_t *volume = rec_round_button(extras, 56, REC_ICON_VOLUME, &font_icons_24, volume_clicked, NULL);
    lv_obj_t *levels = rec_row(extras, 4);
    lv_obj_set_flex_align(levels, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_right(levels, 48, 0);
    for (int i = 0; i < REC_VOLUME_LEVELS; i++) {
        lv_obj_t *bar = lv_obj_create(levels);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 7, 8 + i * 5);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        s.volume_bars[i] = bar;
    }
    lv_obj_t *trash = rec_round_button(extras, 56, REC_ICON_TRASH, &font_icons_24, delete_clicked, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(trash, 0), lv_color_hex(REC_ACCENT), 0);

    rec_hint(root, "PWR volver");
    rec_focus_add(s.play);
    rec_focus_add(fwd);
    rec_focus_add(volume);
    rec_focus_add(trash);
    rec_focus_add(back);
    rec_focus_set(0);
    volume_refresh();

    if (s_autoplay) {
        s_autoplay = false;
        rec_play_start(s_file.name);
    }
    s.timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    refresh_cb(s.timer);
}

static void player_destroy(void)
{
    rec_play_stop();
    if (s.timer != NULL) {
        lv_timer_delete(s.timer);
        s.timer = NULL;
    }
}

const rec_screen_t rec_player_screen = {
    .name = "player",
    .create = player_create,
    .destroy = player_destroy,
};

// ---------- delete confirmation ----------

static lv_obj_t *s_delete_msg;

static void cancel_clicked(lv_event_t *e)
{
    rec_back();
}

static void confirm_clicked(lv_event_t *e)
{
    if (rec_files_delete(s_file.name) == ESP_OK) {
        rec_pop(2); // back to the list, skipping the player of the deleted file
    } else {
        lv_label_set_text(s_delete_msg, "No se pudo borrar");
        lv_obj_set_style_text_color(s_delete_msg, lv_color_hex(REC_WARN), 0);
    }
}

static void delete_create(lv_obj_t *root)
{
    rec_title(root, "Borrar");
    lv_obj_t *icon = rec_label(root, &font_icons_32, REC_ACCENT, REC_ICON_TRASH);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 120);
    char title[40], duration[16];
    rec_files_title(s_file.name, title, sizeof(title));
    rec_format_duration(duration, sizeof(duration), s_file.duration_ms);
    lv_obj_t *question = rec_label(root, &font_barlow_semibold_22, REC_TEXT, "¿Borrar esta grabación?");
    lv_obj_align(question, LV_ALIGN_TOP_MID, 0, 176);
    s_delete_msg = rec_label(root, &font_barlow_18, REC_MUTED, "");
    lv_label_set_text_fmt(s_delete_msg, "%s · %s", title, duration);
    lv_obj_align(s_delete_msg, LV_ALIGN_TOP_MID, 0, 212);

    lv_obj_t *buttons = rec_row(root, 20);
    lv_obj_align(buttons, LV_ALIGN_TOP_MID, 0, 300);
    lv_obj_t *cancel = rec_pill_button(buttons, "Cancelar", REC_SURFACE, cancel_clicked, NULL);
    lv_obj_t *confirm = rec_pill_button(buttons, "Borrar", 0xB42318, confirm_clicked, NULL);
    rec_hint(root, "BOOT elegir · PWR cancelar");
    rec_focus_add(cancel);
    rec_focus_add(confirm);
    rec_focus_set(0);
}

const rec_screen_t rec_delete_screen = {
    .name = "delete",
    .create = delete_create,
};
