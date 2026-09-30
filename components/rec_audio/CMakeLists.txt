idf_component_register(
    SRCS "rec_audio.c" "rec_files.c" "rec_wav.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES fatfs esp_timer waveshare__esp32_s3_touch_amoled_2_06 espressif__esp_codec_dev
)
