#ifndef SDCARD_INIT_H
#define SDCARD_INIT_H

#include "esp_err.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

#include "bsp/esp32_p4_function_ev_board.h"


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 鍒濆鍖?SDMMC 骞舵寕杞藉埌 /sdcard
 * @return ESP_OK 鎴愬姛锛屽叾浠栦负澶辫触
 */
esp_err_t sdcard_init(void);


#ifdef __cplusplus
}
#endif

#endif
