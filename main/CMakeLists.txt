idf_component_register(SRCS "app_main.c" "ui/ui.c" "ui/ui_font_zh_22.c" "camera/camera.c" "sdcard/sdcard_init.cpp" "model_loader/pose_estimator.cpp"
                       INCLUDE_DIRS "." "ui" "camera" "sdcard" "model_loader"
                       REQUIRES esp_video esp32_p4_function_ev_board esp_lvgl_port lvgl usb_host_uvc
                       PRIV_REQUIRES esp_timer)
