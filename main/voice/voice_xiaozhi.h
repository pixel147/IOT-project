#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the upstream XiaoZhi-compatible wake-word, audio and WebSocket path.
 * The module owns the microphone while it is enabled. */
esp_err_t voice_xiaozhi_start(void);

/* Starts one XiaoZhi automatic listening turn from the microphone icon. */
esp_err_t voice_xiaozhi_start_listening(void);

#ifdef __cplusplus
}
#endif
