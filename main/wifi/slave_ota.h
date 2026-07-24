/* ============================================================
 * slave_ota.h — ESP32-C6 协处理器固件 OTA 升级
 *
 * 通过 SDIO 将 P4 flash 中预存的 C6 固件推送到 C6。
 * 不需要 ESP-Prog，只需要 SDIO 链路已建立。
 * ============================================================ */

#ifndef SLAVE_OTA_H
#define SLAVE_OTA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 flash 分区读取 C6 固件并通过 SDIO OTA 推送
 *
 * 需要先调用 wifi_init() 建立 SDIO 链路。
 * 固件存放于 partitions.csv 的 slave_fw 分区。
 *
 * @return ESP_OK 成功 / 其他值 失败
 */
esp_err_t slave_ota_perform(void);

#ifdef __cplusplus
}
#endif

#endif /* SLAVE_OTA_H */
