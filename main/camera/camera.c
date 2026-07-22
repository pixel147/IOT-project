#include "cam.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_video_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

static const char *TAG = "CAM";
#define CAM_DEVICE  BSP_CAMERA_DEVICE
#define NUM_BUFS    3

typedef struct {
    uint8_t *start;
    size_t   length;
} cam_buffer_t;

static struct {
    int          fd;
    bool         running;
    cam_buffer_t buffers[NUM_BUFS];
    uint32_t     width;
    uint32_t     height;
    uint32_t     stride;
    uint32_t     pixel_format;
    uint32_t     buffer_count;
    float        fps_calc;
    int64_t      last_ts;
    cam_frame_cb_t callback;
    TaskHandle_t task;

    /* PSRAM 帧缓冲 - 最新帧，LVGL 安全读取 */
    uint8_t     *fb_psram;
    uint32_t     fb_len;
    volatile bool fb_ready;
} s_cam = { .fd = -1, .fb_psram = NULL };

static void log_fourcc(uint32_t fourcc)
{
    ESP_LOGI(TAG, "V4L2 format: %c%c%c%c (0x%08" PRIx32 ")",
             (char)(fourcc & 0xff), (char)((fourcc >> 8) & 0xff),
             (char)((fourcc >> 16) & 0xff), (char)((fourcc >> 24) & 0xff),
             fourcc);
}

static esp_err_t alloc_psram_fb(uint32_t size)
{
    if (s_cam.fb_psram) {
        heap_caps_free(s_cam.fb_psram);
        s_cam.fb_psram = NULL;
    }
    s_cam.fb_psram = (uint8_t *)heap_caps_aligned_alloc(32, size,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_cam.fb_psram) {
        ESP_LOGE(TAG, "PSRAM fb alloc %u failed", size);
        return ESP_ERR_NO_MEM;
    }
    s_cam.fb_len = size;
    s_cam.fb_ready = false;
    ESP_LOGI(TAG, "PSRAM fb allocated: %u bytes", size);
    return ESP_OK;
}

static void free_psram_fb(void)
{
    if (s_cam.fb_psram) {
        heap_caps_free(s_cam.fb_psram);
        s_cam.fb_psram = NULL;
    }
    s_cam.fb_len = 0;
    s_cam.fb_ready = false;
}

static void cam_capture_task(void *arg)
{
    int fd = s_cam.fd;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_buffer buf;
    uint32_t frame_size = s_cam.stride * s_cam.height;

    ESP_LOGI(TAG, "Camera capture task started");

    while (s_cam.running) {
        memset(&buf, 0, sizeof(buf));
        buf.type   = type;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
            if (errno != EAGAIN && errno != EINTR) {
                ESP_LOGE(TAG, "VIDIOC_DQBUF failed: errno=%d (%s)", errno, strerror(errno));
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if ((buf.flags & V4L2_BUF_FLAG_DONE) && buf.index < s_cam.buffer_count) {
            int64_t now = esp_timer_get_time();
            if (s_cam.last_ts > 0) {
                float dt = (float)(now - s_cam.last_ts) / 1000000.0f;
                if (dt > 0) {
                    float instant_fps = 1.0f / dt;
                    s_cam.fps_calc = s_cam.fps_calc == 0.0f
                                   ? instant_fps
                                   : 0.9f * s_cam.fps_calc + 0.1f * instant_fps;
                }
            }
            s_cam.last_ts = now;

            uint8_t *src = s_cam.buffers[buf.index].start;
            if (buf.bytesused < frame_size) {
                ESP_LOGW(TAG, "Short frame: bytesused=%" PRIu32 ", expected=%" PRIu32,
                         buf.bytesused, frame_size);
            }
            uint32_t bytes = buf.bytesused < frame_size ? buf.bytesused : frame_size;

            /* 从 DMA 缓冲区复制到稳定的 PSRAM 帧缓冲 */
            if (s_cam.fb_psram && bytes == frame_size && bytes <= s_cam.fb_len) {
                memcpy(s_cam.fb_psram, src, bytes);
                s_cam.fb_ready = true;
                if (s_cam.callback) {
                    s_cam.callback(s_cam.fb_psram, bytes,
                                   s_cam.width, s_cam.height, s_cam.stride,
                                   s_cam.pixel_format);
                }
            }
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF failed");
        }
    }

    ESP_LOGI(TAG, "Camera capture task stopped");
    vTaskDelete(NULL);
}

esp_err_t cam_start(uint32_t width, uint32_t height, uint32_t fps, cam_frame_cb_t cb)
{
    esp_err_t ret;
    int fd;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint32_t frame_size = width * height * 2;

    if (s_cam.running) return ESP_ERR_INVALID_STATE;

    s_cam.callback = cb;
    s_cam.width    = width;
    s_cam.height   = height;
    s_cam.fps_calc = 0.0f;
    s_cam.last_ts  = 0;

    /* 通过 BSP 初始化摄像头硬件 */
    ESP_LOGI(TAG, "Initializing camera via BSP...");
    ret = bsp_camera_start(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_camera_start failed: %s", esp_err_to_name(ret));
        goto err_deinit_camera;
    }
    ESP_LOGI(TAG, "BSP camera started");

    vTaskDelay(pdMS_TO_TICKS(200));

    fd = open(CAM_DEVICE, O_RDWR);
    if (fd < 0) {
        ESP_LOGE(TAG, "Cannot open %s: errno=%d (%s)", CAM_DEVICE, errno, strerror(errno));
        ret = ESP_FAIL;
        goto err_deinit_camera;
    }
    s_cam.fd = fd;

    /* 向 MIPI-CSI 视频设备请求 RGB565（ISP 转换 RAW → RGB565） */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type               = type;
    fmt.fmt.pix.width      = width;
    fmt.fmt.pix.height     = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    fmt.fmt.pix.field      = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT RGB565 failed: errno=%d (%s)", errno, strerror(errno));
        ret = ESP_ERR_NOT_SUPPORTED;
        goto err_close;
    }

    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed: errno=%d (%s)", errno, strerror(errno));
        ret = ESP_FAIL;
        goto err_close;
    }

    log_fourcc(fmt.fmt.pix.pixelformat);
    ESP_LOGI(TAG, "Camera format: %ux%u stride=%" PRIu32 " sizeimage=%" PRIu32,
             fmt.fmt.pix.width, fmt.fmt.pix.height,
             fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 &&
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565X) {
        ESP_LOGE(TAG, "Video device did not negotiate RGB565; refusing to feed raw data to LVGL");
        ret = ESP_ERR_NOT_SUPPORTED;
        goto err_close;
    }

    s_cam.width = fmt.fmt.pix.width;
    s_cam.height = fmt.fmt.pix.height;
    s_cam.stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : s_cam.width * 2;
    s_cam.pixel_format = fmt.fmt.pix.pixelformat;
    frame_size = s_cam.stride * s_cam.height;

    /* 仅在确定协商格式后分配 PSRAM 帧缓冲 */
    ret = alloc_psram_fb(frame_size);
    if (ret != ESP_OK) goto err_close;

    /* 请求 MMAP 缓冲区 */
    memset(&req, 0, sizeof(req));
    req.count  = NUM_BUFS;
    req.type   = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2 || req.count > NUM_BUFS) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed or returned invalid count=%" PRIu32,
                 req.count);
        ret = ESP_FAIL;
        goto err_close;
    }
    s_cam.buffer_count = req.count;
    memset(s_cam.buffers, 0, sizeof(s_cam.buffers));

    /* 查询、内存映射并入队每个缓冲区 */
    for (uint32_t i = 0; i < s_cam.buffer_count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type   = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QUERYBUF[%" PRIu32 "] failed: errno=%d (%s)", i, errno, strerror(errno));
            ret = ESP_FAIL;
            goto err_unmap;
        }
        s_cam.buffers[i].length = buf.length;
        s_cam.buffers[i].start  = (uint8_t *)mmap(NULL, buf.length,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (s_cam.buffers[i].start == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%" PRIu32 "] failed: errno=%d (%s)", i, errno, strerror(errno));
            ret = ESP_FAIL;
            goto err_unmap;
        }
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF[%" PRIu32 "] failed: errno=%d (%s)", i, errno, strerror(errno));
            ret = ESP_FAIL;
            goto err_unmap;
        }
    }

    /* 启动视频流 */
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed: errno=%d (%s)", errno, strerror(errno));
        ret = ESP_FAIL;
        goto err_unmap;
    }

    /* 设置帧率（尽力而为） */
    {
        struct v4l2_streamparm sparm;
        memset(&sparm, 0, sizeof(sparm));
        sparm.type = type;
        sparm.parm.capture.timeperframe.numerator   = 1;
        sparm.parm.capture.timeperframe.denominator = fps;
        ioctl(fd, VIDIOC_S_PARM, &sparm);
    }

    /* 诊断：读取实际协商的分辨率和帧率 */
    {
        struct v4l2_format gfmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (ioctl(fd, VIDIOC_G_FMT, &gfmt) == 0) {
            ESP_LOGI(TAG, "Actual format: %c%c%c%c %ux%u stride=%u",
                     (char)(gfmt.fmt.pix.pixelformat & 0xff),
                     (char)((gfmt.fmt.pix.pixelformat >> 8) & 0xff),
                     (char)((gfmt.fmt.pix.pixelformat >> 16) & 0xff),
                     (char)((gfmt.fmt.pix.pixelformat >> 24) & 0xff),
                     gfmt.fmt.pix.width, gfmt.fmt.pix.height,
                     gfmt.fmt.pix.bytesperline);
        }
        struct v4l2_streamparm sparm_diag = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (ioctl(fd, VIDIOC_G_PARM, &sparm_diag) == 0 &&
            sparm_diag.parm.capture.timeperframe.denominator > 0) {
            ESP_LOGI(TAG, "Actual framerate: %u/%u = %.1f fps",
                     sparm_diag.parm.capture.timeperframe.denominator,
                     sparm_diag.parm.capture.timeperframe.numerator,
                     (float)sparm_diag.parm.capture.timeperframe.denominator
                     / sparm_diag.parm.capture.timeperframe.numerator);
        }
    }

    s_cam.running = true;

    /* 创建相机捕获任务（Core 1） */
    xTaskCreatePinnedToCore(cam_capture_task, "cam_capture", 16384, NULL, 5, &s_cam.task, 1);

    ESP_LOGI(TAG, "Camera started: %ux%u @ %u fps", s_cam.width, s_cam.height, fps);
    return ESP_OK;

err_unmap:
    for (uint32_t i = 0; i < s_cam.buffer_count; i++) {
        if (s_cam.buffers[i].start && s_cam.buffers[i].start != MAP_FAILED) {
            munmap(s_cam.buffers[i].start, s_cam.buffers[i].length);
            s_cam.buffers[i].start = NULL;
        }
    }
err_close:
    close(fd);
    s_cam.fd = -1;
    free_psram_fb();
    s_cam.buffer_count = 0;
    return ret;
err_deinit_camera:
    free_psram_fb();
    return ret;
}

void cam_stop(void)
{
    if (!s_cam.running) return;

    s_cam.running = false;

    if (s_cam.task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_cam.task = NULL;
    }

    if (s_cam.fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);

        for (int i = 0; i < NUM_BUFS; i++) {
            if (s_cam.buffers[i].start && s_cam.buffers[i].start != MAP_FAILED) {
                munmap(s_cam.buffers[i].start, s_cam.buffers[i].length);
                s_cam.buffers[i].start = NULL;
            }
        }
        close(s_cam.fd);
        s_cam.fd = -1;
    }

    free_psram_fb();
    ESP_LOGI(TAG, "Camera stopped");
}

bool cam_is_running(void)
{
    return s_cam.running;
}

float cam_get_fps(void)
{
    return (float)s_cam.fps_calc;
}

const uint8_t *cam_get_frame_buffer(uint32_t *len)
{
    if (!s_cam.fb_ready || !s_cam.fb_psram) return NULL;
    if (len) *len = s_cam.fb_len;
    return s_cam.fb_psram;
}

void cam_get_frame_info(uint32_t *w, uint32_t *h)
{
    if (w) *w = s_cam.width;
    if (h) *h = s_cam.height;
}
