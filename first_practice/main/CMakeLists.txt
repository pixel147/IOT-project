idf_component_register(SRCS "app_main.c" "ui/ui.c" "ui/ui_font_zh_22.c" "camera/camera.c"
                       INCLUDE_DIRS "." "ui" "camera"
                       REQUIRES esp_video esp32_p4_function_ev_board esp_lvgl_port lvgl
                       PRIV_REQUIRES esp_timer)
