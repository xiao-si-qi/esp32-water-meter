/**
 * @file wifi_manager.h
 * @brief Wi-Fi STA 连接与断线重连。
 */
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

/** @brief 初始化 NVS/netif/事件循环并启动 STA 连接（非阻塞）。 */
esp_err_t wifi_manager_start(void);

/** @brief 重新载入 NVS 中的凭据并立即重连（网页改配置后调用）。 */
esp_err_t wifi_manager_apply_sta_config(void);

/** @brief 读取当前 STA 的 RSSI。 */
esp_err_t wifi_manager_get_rssi(int *rssi_dbm);

#endif /* WIFI_MANAGER_H */
