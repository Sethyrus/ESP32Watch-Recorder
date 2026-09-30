// Recordings, newest first; opening one plays it.
#include <string.h>

#include "rec_screens.h"

#define MAX_ROWS 50 // LVGL objects live in internal RAM: keep the list bounded

static rec_file_t s_files[MAX_ROWS];
static int s_count;
static char s_focused[REC_NAME_MAX]; // restored when coming back from the player

static void row_clicked(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy(s_focused, s_files[i].name, sizeof(s_focused));
    rec_player_open(&s_files[i]);
}

static lv_obj_t *row(lv_obj_t *parent, int i)
{
    const rec_file_t *file = &s_files[i];

    lv_obj_t *card = lv_button_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 338, 76);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(REC_SURFACE), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1F1F1F), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(REC_BORDER), 0);
    rec_style_focus_ring(card, 3);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 14, 0);
    lv_obj_add_event_cb(card, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 46, 46);
    lv_obj_set_style_radius(badge, 14, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(REC_ACCENT_DIM), 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(rec_label(badge, &font_icons_24, REC_ACCENT, REC_ICON_MIC));

    lv_obj_t *text = lv_obj_create(card);
    lv_obj_remove_style_all(text);
    lv_obj_set_size(text, 196, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, 2, 0);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE);
    char title[40];
    rec_files_title(file->name, title, sizeof(title));
    lv_obj_t *name = rec_label(text, &font_barlow_semibold_22, REC_TEXT, title);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 196);
    char duration[16], size[16];
    rec_format_duration(duration, sizeof(duration), file->duration_ms);
    rec_format_bytes(size, sizeof(size), file->bytes);
    lv_label_set_text_fmt(rec_label(text, &font_barlow_16, REC_MUTED, ""), "%s · %s", duration, size);

    rec_label(card, &font_icons_24, REC_MUTED, REC_ICON_CHEVRON);
    return card;
}

static void list_create(lv_obj_t *root)
{
    rec_title(root, "Grabaciones");
    int total = 0;
    s_count = rec_files_list(s_files, MAX_ROWS, &total);

    if (s_count == 0) {
        lv_obj_t *empty = rec_label(root, &font_barlow_20, REC_MUTED,
                                    rec_files_mounted() ? "Sin grabaciones" : "Sin microSD");
        lv_obj_center(empty);
        rec_hint(root, "PWR volver");
        return;
    }
    rec_hint(root, "BOOT abrir · PWR volver");

    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 360, 350);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_style_pad_ver(list, 4, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    int focus = 0;
    for (int i = 0; i < s_count; i++) {
        rec_focus_add(row(list, i));
        if (strcmp(s_files[i].name, s_focused) == 0) {
            focus = i;
        }
    }
    if (total > s_count) {
        lv_obj_t *more = rec_label(list, &font_barlow_16, REC_MUTED, "");
        lv_label_set_text_fmt(more, "Las %d más recientes de %d", s_count, total);
    }
    rec_focus_set(focus);
}

const rec_screen_t rec_list_screen = {
    .name = "list",
    .create = list_create,
};
