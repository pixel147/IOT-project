#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max faces per frame */
#define FACE_DETECT_MAX_FACES 10

/** One detected face */
typedef struct {
    int x, y, w, h;       // bounding box in pixel coords
    float confidence;      // detection confidence [0,1]
} face_detect_result_t;

/** Results from one frame */
typedef struct {
    face_detect_result_t faces[FACE_DETECT_MAX_FACES];
    int count;
} face_detect_results_t;

/**
 * @brief Initialize face detector (loads models from SD card).
 *        SD card must be mounted at /sdcard.
 */
void face_detect_init(void);

/**
 * @brief Run face detection on an RGB565 frame.
 * @param rgb565   RGB565 pixel buffer
 * @param width    frame width in pixels
 * @param height   frame height in pixels
 * @param results  output — detected faces
 */
void face_detect_run(const uint16_t *rgb565, int width, int height,
                     face_detect_results_t *results);

/**
 * @brief Draw a rectangle on an RGB565 buffer (used for bbox overlay).
 */
void draw_rect_rgb565(uint16_t *buf, int width, int height,
                      int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b, int thickness);

#ifdef __cplusplus
}
#endif
