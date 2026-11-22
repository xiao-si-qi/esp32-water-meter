/**
 * @file mqtt_settings.h
 * @brief MQTT 连接参数的运行时可配置来源。
 *
 * 编译期默认值写在 app_config.h；本模块提供 NVS 覆盖层：
 *   有 NVS 记录 -> 用 NVS（可运行时切换 mqtt/mqtts、换服务器、换账号）
 *   无 NVS 记录 -> 落到编译期默认值
 *
 * 下一阶段做网页配置时，页面只需调用 mqtt_settings_save() + mqtt_manager_restart()，
 * 不需要改动 mqtt_manager 内部逻辑。
 */
#ifndef MQTT_SETTINGS_H
#define MQTT_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define MQTT_SETTINGS_URI_MAX       160
#define MQTT_SETTINGS_USER_MAX       32
#define MQTT_SETTINGS_PASS_MAX       64
#define MQTT_SETTINGS_PREFIX_MAX     32
#define MQTT_SETTINGS_CA_MAX       2048

typedef enum {
    MQTT_TLS_VERIFY_BUNDLE = 0,     /* 使用 ESP-IDF 内置根证书包（推荐，覆盖主流公网 CA） */
    MQTT_TLS_VERIFY_CUSTOM_CA = 1,  /* 使用自备 CA（自签 / 私有 CA 场景） */
    MQTT_TLS_NO_VERIFY = 2,         /* 跳过校验，仅限开发调试 */
} mqtt_tls_verify_t;

typedef struct {
    char              uri[MQTT_SETTINGS_URI_MAX];
    char              username[MQTT_SETTINGS_USER_MAX];
    char              password[MQTT_SETTINGS_PASS_MAX];
    char              client_id_prefix[MQTT_SETTINGS_PREFIX_MAX];
    mqtt_tls_verify_t tls_verify;
    char              ca_cert[MQTT_SETTINGS_CA_MAX];
} mqtt_settings_t;

/**
 * @brief 载入配置：优先 NVS，缺失则用 app_config.h 的默认值。
 * @return ESP_OK 表示命中了 NVS 覆盖，ESP_ERR_NOT_FOUND 表示使用默认值。
 */
esp_err_t mqtt_settings_load(mqtt_settings_t *out);

/** @brief 写入 NVS（不自动重启 MQTT，调用方需再调 mqtt_manager_restart()）。 */
esp_err_t mqtt_settings_save(const mqtt_settings_t *settings);

/** @brief 擦除 NVS 覆盖，恢复编译期默认值。 */
esp_err_t mqtt_settings_reset(void);

/** @brief 按 URI 前缀判断是否走 TLS（mqtts:// 或 wss://）。 */
bool mqtt_settings_is_tls(const mqtt_settings_t *settings);

#endif /* MQTT_SETTINGS_H */
