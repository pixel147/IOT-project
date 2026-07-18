#include "sdcard_init.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

// static const char *TAG = "SDCARD";   // 定义日志标签
#include "bsp/esp32_p4_function_ev_board.h"

static const char *TAG = "SDCARD";

esp_err_t sdcard_init(void) {

    #if CONFIG_IDF_TARGET_ESP32P4
    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    bsp_sdcard_cfg_t sd_cfg = {
        .mount = &mount_cfg,
        .host = NULL,
        .slot = { .sdmmc = NULL },
    };
    esp_err_t ret = bsp_sdcard_sdmmc_mount(&sd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sdcard_sdmmc_mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    sdmmc_card_t *card = bsp_sdcard_get_handle();
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD card mounted at /sdcard");
    return ESP_OK;
    #else
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}
