/**
 * @file wifi_settings.h
 * @brief Wi-Fi 凭据的运行时可配置来源。
 *
 * 与 mqtt_settings 同样的策略：编译期默认值在 app_config.h，
 * NVS 有记录则覆盖，供网页配置写入而无需重新烧录。
 */
#ifndef WIFI_SETTINGS_H
#define WIFI_SETTINGS_H

#include "esp_err.h"

#define WIFI_SETTINGS_SSID_MAX  33
#define WIFI_SETTINGS_PASS_MAX  65

typedef struct {
    char ssid[WIFI_SETTINGS_SSID_MAX];
    char password[WIFI_SETTINGS_PASS_MAX];
} wifi_settings_t;

/** @brief 载入配置：优先 NVS，缺失则用 app_config.h 默认值。 */
esp_err_t wifi_settings_load(wifi_settings_t *out);

/** @brief 写入 NVS（调用方需再触发重连）。 */
esp_err_t wifi_settings_save(const wifi_settings_t *settings);

#endif /* WIFI_SETTINGS_H */
