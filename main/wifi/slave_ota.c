/* ============================================================
 * slave_ota.c — C6 协处理器固件 OTA 升级（SDIO）
 *
 * 固件通过 esptool 烧录到 slave_fw 分区：
 *   esptool.py write_flash <PART_OFFSET> slave_fw.bin
 * ============================================================ */

#include "slave_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_hosted_ota.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include <string.h>

static const char *TAG = "SLAVE_OTA";

#define CHUNK_SIZE  1500
#define PART_LABEL  "slave_fw"

/* ESP 固件头（本地定义，避免引入 bootloader_support 依赖） */
#define ESP_IMAGE_MAGIC  0xE9

typedef struct {
    uint8_t  magic;
    uint8_t  segment_count;
    uint8_t  spi_mode;
    uint8_t  spi_speed_size;
    uint32_t entry_addr;
    uint8_t  wp_pin;
    uint8_t  spi_pin_drv[3];
    uint8_t  reserved[11];
    uint8_t  hash_appended;
} __attribute__((packed)) img_header_t;

typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} __attribute__((packed)) img_segment_t;

esp_err_t slave_ota_perform(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    if (!part) {
        ESP_LOGE(TAG, "Partition '%s' not found", PART_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Found '%s': %" PRIu32 " bytes at 0x%" PRIx32,
             PART_LABEL, part->size, part->address);

    /* SDIO 链路未就绪则跳过 OTA */
    esp_hosted_coprocessor_fwver_t quick_check = {0};
    if (esp_hosted_get_coprocessor_fwversion(&quick_check) != ESP_OK) {
        ESP_LOGI(TAG, "C6 not reachable yet — skipping OTA (will retry next boot)");
        return ESP_ERR_INVALID_STATE;
    }

    /* 解析 ESP 固件头，精确计算大小 */
    img_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
        hdr.magic != ESP_IMAGE_MAGIC) {
        ESP_LOGE(TAG, "Invalid ESP firmware image");
        return ESP_ERR_INVALID_ARG;
    }

    size_t fw_size = sizeof(img_header_t);
    size_t offset = sizeof(img_header_t);
    for (int i = 0; i < hdr.segment_count; i++) {
        img_segment_t seg;
        esp_partition_read(part, offset, &seg, sizeof(seg));
        fw_size += sizeof(seg) + seg.data_len;
        offset  += sizeof(seg) + seg.data_len;
    }
    /* 16 字节对齐 + 1 字节 checksum */
    fw_size = (fw_size + 15) & ~15;
    fw_size += 1;
    /* SHA256 hash（如有） */
    if (hdr.hash_appended) fw_size += 32;

    if (fw_size < 4096 || fw_size > part->size) {
        ESP_LOGE(TAG, "Bad firmware size: %u", (unsigned int)fw_size);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Firmware: %u bytes, %d segments — checking version",
             (unsigned int)fw_size, hdr.segment_count);

    /* 检查 C6 当前版本，已是最新则跳过 */
    esp_hosted_coprocessor_fwver_t slave_ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&slave_ver) == ESP_OK) {
        ESP_LOGI(TAG, "C6 current: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 slave_ver.major1, slave_ver.minor1, slave_ver.patch1);
        /* 我们的固件是 v2.11.7 — C6 版本 >= 2.11.7 则跳过 */
        uint32_t cur = (slave_ver.major1 << 16) | (slave_ver.minor1 << 8) | slave_ver.patch1;
        uint32_t target = (2 << 16) | (11 << 8) | 7;
        if (cur >= target) {
            ESP_LOGI(TAG, "C6 firmware is up-to-date — skipping OTA");
            return ESP_OK;
        }
    } else {
        ESP_LOGW(TAG, "Cannot read C6 version — proceeding with OTA");
    }

    ESP_LOGI(TAG, "Starting OTA (%u bytes, %d segments)…",
             (unsigned int)fw_size, hdr.segment_count);

    /* ---- OTA ---- */
    esp_err_t ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "OTA begin: %s", esp_err_to_name(ret)); return ret; }

    uint8_t chunk[CHUNK_SIZE];
    size_t sent = 0;
    int pct = -1;

    while (sent < fw_size) {
        size_t n = (fw_size - sent > CHUNK_SIZE) ? CHUNK_SIZE : (fw_size - sent);
        esp_partition_read(part, sent, chunk, n);
        ret = esp_hosted_slave_ota_write(chunk, n);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA write @ %u: %s", (unsigned int)sent, esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return ret;
        }
        sent += n;
        int new_pct = (int)(sent * 100 / fw_size);
        if (new_pct != pct) { pct = new_pct; ESP_LOGI(TAG, "OTA: %d%%", pct); }
    }

    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "OTA end: %s", esp_err_to_name(ret)); return ret; }

    ESP_LOGI(TAG, "OTA done — %u bytes. Restart P4 to re-sync with C6.", (unsigned int)sent);
    return ESP_OK;
}
