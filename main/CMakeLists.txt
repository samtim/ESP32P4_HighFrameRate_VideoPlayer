idf_component_register(
    SRCS "main.c" "app_lcd.c" "app_sdcard.c"
    INCLUDE_DIRS "."
    REQUIRES esp_mjpeg_decode esp_driver_sdmmc esp_lcd esp_timer fatfs esp_driver_gpio esp_hw_support
)