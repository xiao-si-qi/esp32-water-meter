/**
 * @file time_sync.h
 * @brief SNTP 网络对时。
 *
 * 存在的唯一理由：TLS 证书校验依赖正确的系统时间。
 * ESP32 上电时 RTC 从 1970 年开始，此时校验证书会得到“证书尚未生效”，
 * MQTTS 握手必然失败且错误信息很难看懂，因此把它单独做成一个模块。
 */
#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>

#include "esp_err.h"

/** @brief 启动 SNTP（幂等，可重复调用）。 */
esp_err_t time_sync_start(void);

/** @brief 系统时间是否已可信（用于 TLS 连接前的门控）。 */
bool time_sync_is_valid(void);

/** @brief 阻塞等待对时完成，超时返回 ESP_ERR_TIMEOUT。 */
esp_err_t time_sync_wait_valid(uint32_t timeout_ms);

#endif /* TIME_SYNC_H */
