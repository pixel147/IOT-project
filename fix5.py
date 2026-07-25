text = open("main/voice/voice_xiaozhi.cpp", "r", encoding="utf-8").read()

# Remove debug logging, make i2s_channel_read the primary read path
old_block = '''        /* Read PCM audio from codec (try codec API first, fallback to raw I2S) */
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

new_block = '''        /* Read PCM directly from I2S RX (codec_dev_read returns 0 on this board) */
        size_t i2s_bytes = 0;
        esp_err_t i2s_ret = i2s_channel_read(s_voice.rx, afe_buf, needed, &i2s_bytes, pdMS_TO_TICKS(50));
        if (i2s_ret == ESP_OK && i2s_bytes > 0) {
            s_voice.afe_iface->feed(s_voice.afe, afe_buf);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }'''

if old_block in text:
    text = text.replace(old_block, new_block, 1)
    open("main/voice/voice_xiaozhi.cpp", "w", encoding="utf-8").write(text)
    print("Replaced with pure I2S channel read!")
else:
    print("Pattern not found")
    idx = text.find("i2s_channel_read")
    print(f"i2s_channel_read found at {idx}" if idx >= 0 else "Not found")
