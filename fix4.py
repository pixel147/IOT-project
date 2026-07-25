# Read current file
text = open("main/voice/voice_xiaozhi.cpp", "r", encoding="utf-8").read()

# Find the codec read section and replace with dual-mode read (codec first, then i2s direct)
old_section = '''        /* Read PCM audio from codec (blocking, with timeout) */
        int ret = esp_codec_dev_read(s_voice.codec, afe_buf, needed);
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

new_section = '''        /* Read PCM audio from codec (try codec API first, fallback to raw I2S) */
        int ret = esp_codec_dev_read(s_voice.codec, afe_buf, needed);
        if (ret > 0) {
            s_voice.afe_iface->feed(s_voice.afe, afe_buf);
        } else {
            static int fail_ct2 = 0;
            if (++fail_ct2 == 1) {
                ESP_LOGI(TAG, "codec_read=0, trying raw I2S read");
            }
            /* Fallback: read directly from I2S RX channel */
            size_t i2s_bytes = 0;
            esp_err_t i2s_ret = i2s_channel_read(s_voice.rx, afe_buf, needed, &i2s_bytes, pdMS_TO_TICKS(50));
            if (i2s_ret == ESP_OK && i2s_bytes > 0) {
                if (fail_ct2 <= 5 || (fail_ct2 % 50 == 0)) {
                    ESP_LOGI(TAG, "i2s_read: bytes=%d ct=%d", (int)i2s_bytes, fail_ct2);
                }
                s_voice.afe_iface->feed(s_voice.afe, afe_buf);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }'''

if old_section in text:
    text = text.replace(old_section, new_section, 1)
    open("main/voice/voice_xiaozhi.cpp", "w", encoding="utf-8").write(text)
    print("Added fallback I2S direct read!")
else:
    print("Pattern not found")
    idx = text.find("codec_read")
    if idx >= 0:
        print(f"Found 'codec_read' at {idx}: ...{text[idx:idx+200]}...")
