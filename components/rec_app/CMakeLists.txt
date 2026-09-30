file(GLOB FONT_SRCS "fonts/*.c")
idf_component_register(
    SRCS "rec_app.c" "rec_ui.c" "rec_record.c" "rec_list.c" "rec_player.c"
         ${FONT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES watch_board lvgl
    PRIV_REQUIRES rec_audio esp_timer nvs_flash waveshare__esp32_s3_touch_amoled_2_06
)
