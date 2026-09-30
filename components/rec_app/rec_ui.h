#pragma once

// Recorder UI toolkit: theme, screen stack, button focus and widgets. A trimmed copy
// of the launcher's os_ui (ESP32Watch-Launcher components/launcher/os.h), with the
// same look and button handling. Every rec_* call that touches LVGL runs with the
// LVGL lock held: the system task takes it before calling into screens.

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// ---- Theme ----
#define REC_BG 0x000000
#define REC_TEXT 0xF4F1EA
#define REC_TEXT_2 0xC9C6BE
#define REC_MUTED 0x8C8A84
#define REC_SURFACE 0x121212
#define REC_BORDER 0x2A2A2A
#define REC_TRACK 0x1C1C1C
#define REC_ACCENT 0xFF4D4F     // recording red
#define REC_ACCENT_DIM 0x3A1414 // red-tinted surface
#define REC_FOCUS 0x4FD1C5      // focus ring: the launcher's turquoise
#define REC_WARN 0xF2A541
#define REC_FOCUS_STATE LV_STATE_USER_1

LV_FONT_DECLARE(font_digits_72);
LV_FONT_DECLARE(font_title_30);
LV_FONT_DECLARE(font_barlow_16);
LV_FONT_DECLARE(font_barlow_18);
LV_FONT_DECLARE(font_barlow_20);
LV_FONT_DECLARE(font_barlow_semibold_22);
LV_FONT_DECLARE(font_icons_24);
LV_FONT_DECLARE(font_icons_32);

// Lucide glyphs in font_icons_24/32 (UTF-8).
#define REC_ICON_MIC "\xee\x84\x98"
#define REC_ICON_SQUARE "\xee\x85\xa7"
#define REC_ICON_TRASH "\xee\x86\x8e"
#define REC_ICON_LIST "\xee\x84\x86"
#define REC_ICON_PLAY "\xee\x84\xbc"
#define REC_ICON_PAUSE "\xee\x84\xae"
#define REC_ICON_BACK10 "\xee\x85\x88"    // rotate-ccw
#define REC_ICON_FWD10 "\xee\x85\x89"     // rotate-cw
#define REC_ICON_VOLUME "\xee\x86\xab"
#define REC_ICON_CHECK "\xee\x81\xac"
#define REC_ICON_X "\xee\x86\xb2"
#define REC_ICON_ALERT "\xee\x86\x93"
#define REC_ICON_CHEVRON "\xee\x81\xaf"
#define REC_ICON_DRIVE "\xee\x83\xad"

// ---- Screens ----
typedef struct {
    const char *name;
    void (*create)(lv_obj_t *root); // build the screen under root (black, 410x502)
    void (*destroy)(void);          // optional: forget pointers into the deleted tree
    void (*tick)(void);             // optional: once a second, after creation and after waking
    bool (*on_boot)(void);          // optional: BOOT short press; false = click the focus
    bool (*on_pwr)(void);           // optional: PWR short press; false = back
    bool keep_awake;                // no screen-off timeout while shown
} rec_screen_t;

void rec_push(const rec_screen_t *screen);
void rec_back(void);
void rec_pop(int count);        // back count screens at once (never past the root)
void rec_request_sleep(void);   // turn the screen off as soon as the system task can

// Screen timeout (s) and brightness come from the launcher's settings.
void rec_ui_start(const rec_screen_t *root, int screen_timeout_s);

// ---- Button focus ----
// Focusable objects of the current screen, in order: BOOT long press moves the focus
// ring to the next one, BOOT short press clicks it. Cleared on every screen change.
void rec_focus_add(lv_obj_t *obj);
void rec_focus_set(int index);
void rec_focus_set_obj(lv_obj_t *obj);

// ---- Widgets ----
lv_obj_t *rec_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);
// Title row "<title>  HH:MM" at the top; returns the row.
lv_obj_t *rec_title(lv_obj_t *root, const char *title);
// Bottom hint line ("BOOT abrir · PWR volver").
lv_obj_t *rec_hint(lv_obj_t *root, const char *text);
// Round icon button; diameter px, icon font 24 or 32. Focus ring included.
lv_obj_t *rec_round_button(lv_obj_t *parent, int diameter, const char *icon, const lv_font_t *font,
                           lv_event_cb_t on_click, void *user);
// Pill button with text; fill = background colour.
lv_obj_t *rec_pill_button(lv_obj_t *parent, const char *text, uint32_t fill, lv_event_cb_t on_click,
                          void *user);
void rec_style_focus_ring(lv_obj_t *obj, int width);
// Transparent container, sized to content, flex row, centred.
lv_obj_t *rec_row(lv_obj_t *parent, int gap);

// "1:05" or "1:02:03".
void rec_format_duration(char *out, size_t size, uint32_t ms);
// "4,9 MB" / "812 KB" / "1,8 GB".
void rec_format_bytes(char *out, size_t size, uint64_t bytes);
