idf_component_register(
    SRCS 
        "main.c" 
        "app_lcd.c" 
        "app_sdcard.c"
        "app_es8311.c"
        "app_audio.c"
        "app_keys.c"
        "app_display.c"
        "app_video.c"
    INCLUDE_DIRS 
        "."
    REQUIRES esp_mjpeg_decode esp_driver_sdmmc esp_lcd esp_timer fatfs esp_driver_gpio esp_hw_support esp_driver_i2s esp_driver_i2c esp_ringbuf minimp3 espressif__esp_audio_effects
)
