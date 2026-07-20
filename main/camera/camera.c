#include "cam.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "driver/jpeg_decode.h"
#include <linux/videodev2.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "UVC_CAM";

#define UVC_FRAME_BUFFER_COUNT  3
#define UVC_FRAME_QUEUE_LENGTH  UVC_FRAME_BUFFER_COUNT
#define UVC_TASK_PRIORITY       10
#define UVC_FRAME_SIZE          (200 * 1024)  /* MJPG 640×480 ~50KB, 200KB 留足余量 */
#define OUT_TARGET_W            640
#define OUT_TARGET_H            480
#define RGB565_BUF_SIZE         (OUT_TARGET_W * OUT_TARGET_H * 2)
#define JPEG_WORK_BUF_SIZE      (32 * 1024)

static struct {
    bool running;
    bool usb_host_installed;
    bool uvc_driver_installed;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    float fps_calc;
    int64_t last_frame_time;
    cam_frame_cb_t callback;
    uint8_t *rgb565_buffer;
    uint32_t rgb565_buffer_len;
    volatile bool frame_ready;
    uvc_host_stream_hdl_t stream;
    QueueHandle_t frame_queue;
    SemaphoreHandle_t format_ready;
    TaskHandle_t processing_task;
    uvc_host_stream_format_t selected_format;
    uint8_t selected_dev_addr;
    uint8_t selected_stream_index;
    bool format_found;
    bool format_is_mjpg;
    jpeg_decoder_handle_t jpeg_dec;
    uint8_t *mjpg_frame_buf[2];     /* 乒乓：回调写一个，解码任务读另一个 */
    uint32_t mjpg_frame_len[2];
    volatile int mjpg_write_idx;    /* 回调写入目标 */
    uint8_t *jpeg_out_buf;
    uint32_t jpeg_out_size;
} s_cam;

static uint8_t clamp_to_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static float frame_interval_to_fps(uint32_t interval)
{
    return interval == 0 ? 0.0f : 10000000.0f / (float)interval;
}

/* 从相机描述符中选择不超过请求尺寸的最高分辨率 YUY2 格式。 */
static bool select_yuy2_format(const uvc_host_frame_info_t *frame_info,
                               size_t frame_info_count,
                               uint32_t max_width, uint32_t max_height,
                               uvc_host_stream_format_t *selected_format)
{
    uint32_t best_pixels = 0;
    bool found = false;

    for (size_t i = 0; i < frame_info_count; i++) {
        const uvc_host_frame_info_t *candidate = &frame_info[i];
        const uint32_t pixels = candidate->h_res * candidate->v_res;

        if (candidate->format != UVC_VS_FORMAT_YUY2 ||
            candidate->h_res > max_width || candidate->v_res > max_height ||
            candidate->default_interval == 0 || pixels < best_pixels) {
            continue;
        }

        selected_format->h_res = candidate->h_res;
        selected_format->v_res = candidate->v_res;
        selected_format->fps = frame_interval_to_fps(candidate->default_interval);
        selected_format->format = UVC_VS_FORMAT_YUY2;
        best_pixels = pixels;
        found = true;
    }

    return found;
}

/* 从相机描述符中选择 MJPG 格式，FPS 不超过 30。 */
static bool select_mjpeg_format(const uvc_host_frame_info_t *frame_info,
                                size_t frame_info_count,
                                uint32_t max_width, uint32_t max_height,
                                uvc_host_stream_format_t *selected_format)
{
    uint32_t best_pixels = 0;
    bool found = false;
    for (size_t i = 0; i < frame_info_count; i++) {
        const uvc_host_frame_info_t *c = &frame_info[i];
        if (c->format != UVC_VS_FORMAT_MJPEG ||
            c->h_res > max_width || c->v_res > max_height ||
            c->default_interval == 0 ||
            c->h_res * c->v_res < best_pixels) continue;
        selected_format->h_res = c->h_res;
        selected_format->v_res = c->v_res;
        float fps = frame_interval_to_fps(c->default_interval);
        selected_format->fps = fps > 30.0f ? 30.0f : fps;
        selected_format->format = UVC_VS_FORMAT_MJPEG;
        best_pixels = c->h_res * c->v_res;
        found = true;
    }
    return found;
}

/* UVC 设备连接后读取描述符，优先 MJPG，其次 YUY2。 */
static void uvc_driver_event_callback(const uvc_host_driver_event_data_t *event,
                                      void *user_ctx)
{
    (void)user_ctx;

    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED ||
        event->device_connected.uvc_stream_index != 0 ||
        !s_cam.format_ready) {
        return;
    }

    size_t frame_info_count = event->device_connected.frame_info_num;
    uvc_host_frame_info_t *frame_info = calloc(frame_info_count, sizeof(*frame_info));
    if (!frame_info) {
        ESP_LOGE(TAG, "UVC 格式描述符内存分配失败");
        xSemaphoreGive(s_cam.format_ready);
        return;
    }

    const esp_err_t ret = uvc_host_get_frame_list(
        event->device_connected.dev_addr,
        event->device_connected.uvc_stream_index,
        (uvc_host_frame_info_t (*)[])frame_info, &frame_info_count);
    if (ret == ESP_OK) {
        for (size_t i = 0; i < frame_info_count; i++) {
            ESP_LOGI(TAG, "UVC 格式: type=%d, %ux%u @ %.1f fps",
                     frame_info[i].format, frame_info[i].h_res,
                     frame_info[i].v_res,
                     frame_interval_to_fps(frame_info[i].default_interval));
        }

        s_cam.format_found = select_mjpeg_format(frame_info, frame_info_count,
                                                  s_cam.width, s_cam.height,
                                                  &s_cam.selected_format);
        s_cam.format_is_mjpg = true;
        if (!s_cam.format_found) {
            s_cam.format_found = select_yuy2_format(frame_info, frame_info_count,
                                                     s_cam.width, s_cam.height,
                                                     &s_cam.selected_format);
            s_cam.format_is_mjpg = false;
        }
        if (s_cam.format_found) {
            s_cam.selected_dev_addr = event->device_connected.dev_addr;
            s_cam.selected_stream_index = event->device_connected.uvc_stream_index;
        }
    } else {
        ESP_LOGE(TAG, "读取 UVC 格式描述符失败: %s", esp_err_to_name(ret));
    }

    free(frame_info);
    xSemaphoreGive(s_cam.format_ready);
}

/* 将 UVC 的 YUY2 数据转换为 LVGL 与推理使用的 RGB565 小端格式。 */
static void yuy2_to_rgb565(const uint8_t *yuy2, uint16_t *rgb565,
                            uint32_t width, uint32_t height)
{
    const uint32_t pixel_count = width * height;

    for (uint32_t i = 0; i < pixel_count; i += 2) {
        const int y0 = yuy2[i * 2];
        const int u = yuy2[i * 2 + 1] - 128;
        const int y1 = yuy2[i * 2 + 2];
        const int v = yuy2[i * 2 + 3] - 128;
        const int c0 = y0 - 16;
        const int c1 = y1 - 16;

        const uint8_t r0 = clamp_to_u8((298 * c0 + 409 * v + 128) >> 8);
        const uint8_t g0 = clamp_to_u8((298 * c0 - 100 * u - 208 * v + 128) >> 8);
        const uint8_t b0 = clamp_to_u8((298 * c0 + 516 * u + 128) >> 8);
        const uint8_t r1 = clamp_to_u8((298 * c1 + 409 * v + 128) >> 8);
        const uint8_t g1 = clamp_to_u8((298 * c1 - 100 * u - 208 * v + 128) >> 8);
        const uint8_t b1 = clamp_to_u8((298 * c1 + 516 * u + 128) >> 8);

        rgb565[i] = (uint16_t)(((r0 >> 3) << 11) | ((g0 >> 2) << 5) | (b0 >> 3));
        rgb565[i + 1] = (uint16_t)(((r1 >> 3) << 11) | ((g1 >> 2) << 5) | (b1 >> 3));
    }
}

/* USB Host 库必须持续处理事件，否则无法枚举或释放 UVC 设备。 */
static void usb_host_event_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* UVC 驱动任务只入队帧指针，耗时转换和应用回调由处理任务完成。 */
static bool uvc_frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;

    if (s_cam.format_is_mjpg) {
        int w = s_cam.mjpg_write_idx;
        if (s_cam.mjpg_frame_buf[w] && frame->data_len <= UVC_FRAME_SIZE) {
            memcpy(s_cam.mjpg_frame_buf[w], frame->data, frame->data_len);
            s_cam.mjpg_frame_len[w] = frame->data_len;
            s_cam.frame_ready = true;
        }
        return true;
    }

    if (!s_cam.running || !s_cam.frame_queue) {
        return true;
    }

    if (xQueueSendToBack(s_cam.frame_queue, &frame, 0) != pdPASS) {
        return true;
    }

    return false;
}

/* 记录传输异常与拔出事件；拔出后等待用户重新调用 cam_start。 */
static void uvc_stream_event_callback(const uvc_host_stream_event_data_t *event,
                                      void *user_ctx)
{
    (void)user_ctx;

    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "UVC 传输错误: %s", esp_err_to_name(event->transfer_error.error));
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "UVC 相机已拔出");
        s_cam.running = false;
        s_cam.stream = NULL;
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "UVC 帧缓冲溢出");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGW(TAG, "UVC 帧缓冲不足");
        break;
#ifdef UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case UVC_HOST_DEVICE_SUSPENDED:
        ESP_LOGI(TAG, "UVC 相机已挂起");
        break;
    case UVC_HOST_DEVICE_RESUMED:
        ESP_LOGI(TAG, "UVC 相机已恢复");
        break;
#endif
    default:
        break;
    }
}

/* 转换完成后再归还 UVC 原始帧，保证驱动不会重用仍在处理的缓冲区。 */
static void uvc_frame_processing_task(void *arg)
{
    (void)arg;

    /* MJPG: 初始化 ESP-IDF JPEG 解码器（替代 esp_new_jpeg，兼容更多相机） */
    if (s_cam.format_is_mjpg) {
        jpeg_decode_engine_cfg_t eng_cfg = { .timeout_ms = 100 };
        if (jpeg_new_decoder_engine(&eng_cfg, &s_cam.jpeg_dec) != ESP_OK) {
            ESP_LOGE(TAG, "IDF JPEG 解码器创建失败");
            s_cam.jpeg_dec = NULL;
        }
    }

    while (true) {
        /* ---- MJPG 路径 ---- */
        if (s_cam.format_is_mjpg) {
            if (!s_cam.frame_ready) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            s_cam.frame_ready = false;

            const int64_t now = esp_timer_get_time();
            if (s_cam.last_frame_time > 0) {
                float dt = (float)(now - s_cam.last_frame_time) / 1000000.0f;
                if (dt > 0) {
                    float ifps = 1.0f / dt;
                    s_cam.fps_calc = s_cam.fps_calc == 0.0f
                                   ? ifps : 0.9f * s_cam.fps_calc + 0.1f * ifps;
                }
            }
            s_cam.last_frame_time = now;

            int r = s_cam.mjpg_write_idx;  /* 当前写完的缓冲 */
            s_cam.mjpg_write_idx ^= 1;       /* 交换：回调写另一块 */
            if (s_cam.jpeg_dec && s_cam.mjpg_frame_len[r] > 0) {
                uint32_t len = s_cam.mjpg_frame_len[r];
                uint8_t  *src = s_cam.mjpg_frame_buf[r];

                /* 获取 JPEG 尺寸 */
                jpeg_decode_picture_info_t pic_info;
                /* ① 解码前：写回输入数据到 PSRAM */
                size_t align_in = (len + 63) & ~63;
                esp_cache_msync(src, align_in,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

                if (jpeg_decoder_get_info(src, len, &pic_info) != ESP_OK) {
                    ESP_LOGW(TAG, "JPEG get_info err (len=%u)", len);
                    continue;
                }

                /* 按实际分辨率分配输出缓冲 */
                uint32_t need = pic_info.width * pic_info.height * 2;
                if (need > s_cam.jpeg_out_size || !s_cam.jpeg_out_buf) {
                    free(s_cam.jpeg_out_buf);
                    s_cam.jpeg_out_buf = heap_caps_aligned_alloc(64, need,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
                    if (!s_cam.jpeg_out_buf) { s_cam.jpeg_out_size = 0; continue; }
                    s_cam.jpeg_out_size = need;
                }

                /* ② 解码前：失效输出缓冲缓存（让 DMA 直接写 PSRAM） */
                esp_cache_msync(s_cam.jpeg_out_buf, (need + 63) & ~63,
                                ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

                /* IDF JPEG 解码 */
                jpeg_decode_cfg_t dec_cfg = {
                    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                };
                uint32_t out_len = s_cam.jpeg_out_size;
                esp_err_t ret = jpeg_decoder_process(s_cam.jpeg_dec, &dec_cfg,
                                        src, len,
                                        s_cam.jpeg_out_buf, s_cam.jpeg_out_size, &out_len);
                if (ret == ESP_OK && s_cam.callback && out_len > 0) {
                    /* ③ 解码后：刷新 CPU 缓存读 PSRAM */
                    esp_cache_msync(s_cam.jpeg_out_buf, (out_len + 63) & ~63,
                                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

                    int dw = OUT_TARGET_W, dh = OUT_TARGET_H;
                    int sx = (int)pic_info.width / dw;
                    int sy = (int)pic_info.height / dh;
                    uint16_t *px_src = (uint16_t *)s_cam.jpeg_out_buf;
                    uint16_t *dst   = (uint16_t *)s_cam.rgb565_buffer;
                    for (int y = 0; y < dh; y++) {
                        uint16_t *srow = px_src + (y * sy) * (int)pic_info.width;
                        uint16_t *drow = dst + y * dw;
                        for (int x = 0; x < dw; x++)
                            drow[x] = srow[x * sx];
                    }
                    esp_cache_msync(s_cam.rgb565_buffer, (dw * dh * 2 + 63) & ~63,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
                    s_cam.callback(s_cam.rgb565_buffer, (uint32_t)(dw * dh * 2),
                                   (uint32_t)dw, (uint32_t)dh,
                                   (uint32_t)(dw * 2), V4L2_PIX_FMT_RGB565);
                } else if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "JPEG err 0x%x %ux%u (len=%u)",
                             ret, pic_info.width, pic_info.height, len);
                }
            }
            continue;
        }

        /* ---- YUY2 路径 ---- */
        const uvc_host_frame_t *frame = NULL;
        if (xQueueReceive(s_cam.frame_queue, &frame, portMAX_DELAY) != pdPASS)
            continue;

        if (s_cam.running && frame &&
            frame->vs_format.format == UVC_VS_FORMAT_YUY2 &&
            s_cam.rgb565_buffer) {
            const int64_t now = esp_timer_get_time();
            if (s_cam.last_frame_time > 0) {
                float dt = (float)(now - s_cam.last_frame_time) / 1000000.0f;
                if (dt > 0) {
                    float ifps = 1.0f / dt;
                    s_cam.fps_calc = s_cam.fps_calc == 0.0f
                                   ? ifps : 0.9f * s_cam.fps_calc + 0.1f * ifps;
                }
            }
            s_cam.last_frame_time = now;
            yuy2_to_rgb565(frame->data, (uint16_t *)s_cam.rgb565_buffer,
                            frame->vs_format.h_res, frame->vs_format.v_res);
            if (s_cam.callback) {
                s_cam.callback(s_cam.rgb565_buffer,
                               frame->vs_format.h_res * frame->vs_format.v_res * 2,
                               frame->vs_format.h_res, frame->vs_format.v_res,
                               frame->vs_format.h_res * 2, V4L2_PIX_FMT_RGB565);
            }
        }
        if (s_cam.stream)
            uvc_host_frame_return(s_cam.stream, (uvc_host_frame_t *)frame);
    }
}

static esp_err_t allocate_rgb565_buffer(uint32_t size)
{
    s_cam.rgb565_buffer = heap_caps_aligned_alloc(16, size,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_cam.rgb565_buffer) {
        ESP_LOGE(TAG, "PSRAM RGB565 帧缓冲分配失败: %u 字节", (unsigned int)size);
        return ESP_ERR_NO_MEM;
    }

    s_cam.rgb565_buffer_len = size;
    return ESP_OK;
}

esp_err_t cam_start(uint32_t width, uint32_t height, uint32_t fps, cam_frame_cb_t cb)
{
    if (s_cam.running || s_cam.uvc_driver_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width == 0 || height == 0 || fps == 0 || (width & 1U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cam.callback = cb;
    s_cam.width = width;
    s_cam.height = height;
    s_cam.stride = width * 2;
    s_cam.fps_calc = 0.0f;
    s_cam.last_frame_time = 0;
    s_cam.frame_ready = false;
    s_cam.format_found = false;
    s_cam.selected_dev_addr = 0;
    s_cam.selected_stream_index = 0;

    s_cam.format_ready = xSemaphoreCreateBinary();
    if (!s_cam.format_ready) {
        ESP_LOGE(TAG, "UVC 格式同步信号量创建失败");
        return ESP_ERR_NO_MEM;
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Host 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    s_cam.usb_host_installed = true;

    if (xTaskCreatePinnedToCore(usb_host_event_task, "usb_host", 4096, NULL,
                                UVC_TASK_PRIORITY, NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "USB Host 事件任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    const uvc_host_driver_config_t driver_config = {
        .driver_task_stack_size = 6 * 1024,
        .driver_task_priority = UVC_TASK_PRIORITY + 1,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
        .event_cb = uvc_driver_event_callback,
    };
    ret = uvc_host_install(&driver_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UVC 驱动初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    s_cam.uvc_driver_installed = true;

    ESP_LOGI(TAG, "等待 UVC 相机连接并读取格式描述符");
    if (xSemaphoreTake(s_cam.format_ready, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "等待 UVC 相机连接超时");
        return ESP_ERR_TIMEOUT;
    }
    if (!s_cam.format_found) {
        ESP_LOGE(TAG, "UVC 相机不支持不超过 %ux%u 的 YUY2 格式", width, height);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_cam.width = s_cam.selected_format.h_res;
    s_cam.height = s_cam.selected_format.v_res;
    s_cam.stride = s_cam.width * 2;

    /* MJPG: 按解码后 RGB565 尺寸分配；YUY2: 按原始尺寸 */
    ret = allocate_rgb565_buffer(s_cam.format_is_mjpg
                                  ? RGB565_BUF_SIZE
                                  : s_cam.stride * s_cam.height);
    if (ret != ESP_OK) return ret;

    if (s_cam.format_is_mjpg) {
        for (int i = 0; i < 2; i++) {
            s_cam.mjpg_frame_buf[i] = heap_caps_aligned_alloc(64, UVC_FRAME_SIZE,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        }
    }

    s_cam.frame_queue = xQueueCreate(UVC_FRAME_QUEUE_LENGTH, sizeof(uvc_host_frame_t *));
    if (!s_cam.frame_queue) return ESP_ERR_NO_MEM;

    uint32_t max_frame_sz = s_cam.format_is_mjpg
                            ? UVC_FRAME_SIZE
                            : s_cam.stride * s_cam.height;

    const uvc_host_stream_config_t stream_config = {
        .event_cb = uvc_stream_event_callback,
        .frame_cb = uvc_frame_callback,
        .usb = {
            .dev_addr = s_cam.selected_dev_addr,
            .vid = UVC_HOST_ANY_VID,
            .pid = UVC_HOST_ANY_PID,
            .uvc_stream_index = s_cam.selected_stream_index,
        },
        .vs_format = s_cam.selected_format,
        .advanced = {
            .number_of_frame_buffers = UVC_FRAME_BUFFER_COUNT,
            .frame_size = max_frame_sz,
            .frame_heap_caps = MALLOC_CAP_SPIRAM,
            .number_of_urbs = 4,
            .urb_size = 16 * 1024,
        },
    };

    ESP_LOGI(TAG, "打开 UVC 视频流: %s %ux%u @ %.1f fps",
             s_cam.format_is_mjpg ? "MJPG" : "YUY2",
             s_cam.width, s_cam.height, s_cam.selected_format.fps);
    ret = uvc_host_stream_open(&stream_config, pdMS_TO_TICKS(5000), &s_cam.stream);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "无法打开 UVC 视频流: %s", esp_err_to_name(ret));
        return ret;
    }

    if (xTaskCreatePinnedToCore(uvc_frame_processing_task, "uvc_process", 8192, NULL,
                                UVC_TASK_PRIORITY, &s_cam.processing_task,
                                tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "UVC 帧处理任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_cam.running = true;
    ret = uvc_host_stream_start(s_cam.stream);
    if (ret != ESP_OK) {
        s_cam.running = false;
        ESP_LOGE(TAG, "UVC 视频流启动失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "UVC 相机已启动: YUY2 %ux%u @ %.1f fps",
             s_cam.width, s_cam.height, s_cam.selected_format.fps);
    return ESP_OK;
}

void cam_stop(void)
{
    s_cam.running = false;

    if (s_cam.stream) {
        uvc_host_stream_stop(s_cam.stream);
        uvc_host_stream_close(s_cam.stream);
        s_cam.stream = NULL;
    }

    if (s_cam.jpeg_dec) {
        jpeg_del_decoder_engine(s_cam.jpeg_dec);
        s_cam.jpeg_dec = NULL;
    }
    free(s_cam.jpeg_out_buf);
    s_cam.jpeg_out_buf = NULL;
    s_cam.jpeg_out_size = 0;
    for (int i = 0; i < 2; i++) {
        free(s_cam.mjpg_frame_buf[i]);
        s_cam.mjpg_frame_buf[i] = NULL;
    }

    s_cam.frame_ready = false;
    ESP_LOGI(TAG, "UVC 相机已停止");
}

bool cam_is_running(void)
{
    return s_cam.running;
}

float cam_get_fps(void)
{
    return s_cam.fps_calc;
}

const uint8_t *cam_get_frame_buffer(uint32_t *len)
{
    if (!s_cam.frame_ready || !s_cam.rgb565_buffer) {
        return NULL;
    }
    if (len) {
        *len = s_cam.rgb565_buffer_len;
    }
    return s_cam.rgb565_buffer;
}

void cam_get_frame_info(uint32_t *w, uint32_t *h)
{
    if (w) {
        *w = s_cam.width;
    }
    if (h) {
        *h = s_cam.height;
    }
}
