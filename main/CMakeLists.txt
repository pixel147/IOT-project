idf_component_register(SRCS "app_main.c" "ui/ui.c" "ui/home.c" "ui/settings.c" "ui/chat.c" "ui_font_zh_22.c" "camera/camera.c"
                       "voice/voice_xiaozhi.cpp"
                       "sdcard/sdcard_init.cpp"
                       "model_loader/face_detect_wrapper.cpp"
                       "model_loader/emotion_espdl.cpp"
                       "wifi/wifi.c" "wifi/slave_ota.c"
                       "llm/llm_client.c"
                       INCLUDE_DIRS "." "ui" "camera" "sdcard" "model_loader" "wifi" "llm" "voice"
                       REQUIRES esp_video esp32_p4_function_ev_board esp_lvgl_port lvgl human_face_detect esp_partition
                       PRIV_REQUIRES esp_timer esp-dl nvs_flash esp_wifi esp_netif esp_event espressif__esp_websocket_client
                                     esp_driver_i2s esp_driver_i2c esp_codec_dev espressif__esp_audio_codec
                                     esp_http_client)
