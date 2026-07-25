text = open("main/voice/voice_xiaozhi.cpp", "r", encoding="utf-8").read()
old_read = '''        int ret = esp_codec_dev_read(s_voice.codec, afe_buf, needed);
        if (ret > 0) {
            /* Feed raw PCM into AFE for VAD + WakeNet processing */
            s_voice.afe_iface->feed(s_voice.afe, afe_buf);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }'''
new_read = '''        int ret = esp_codec_dev_read(s_voice.codec, afe_buf, needed);
        if (ret > 0) {
            /* Feed raw PCM into AFE for VAD + WakeNet processing */
            s_voice.afe_iface->feed(s_voice.afe, afe_buf);
        } else {
            static int fail_ct = 0;
            if (++fail_ct <= 5 || (fail_ct % 100 == 0)) {
                ESP_LOGI(TAG, "codec_read: ret=%d expect=%d ct=%d", ret, needed, fail_ct);
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }'''
if old_read in text:
    text = text.replace(old_read, new_read, 1)
    open("main/voice/voice_xiaozhi.cpp", "w", encoding="utf-8").write(text)
    print("Fixed!")
else:
    print("Pattern not found")
