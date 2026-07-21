idf_component_register(SRCS "app_main.c" "ui/ui.c" "ui_font_zh_22.c" "camera/camera.c"
                       "model_loader/pose_estimator.cpp" "sdcard/sdcard_init.cpp"
                       INCLUDE_DIRS "." "ui" "camera" "model_loader" "sdcard"
                       REQUIRES esp_video esp32_p4_function_ev_board esp_lvgl_port lvgl usb_host_uvc esp_driver_jpeg esp-dl
                       PRIV_REQUIRES esp_timer)
