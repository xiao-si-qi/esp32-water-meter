#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_sntp.h"

#include "app_config.h"
#include "time_sync.h"

static const char *TAG = "time";

static bool s_started = false;

esp_err_t time_sync_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* 时区只影响日志可读性，不参与 TLS 校验 */
    setenv("TZ", APP_TIME_ZONE, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, APP_SNTP_SERVER_PRIMARY);
    esp_sntp_setservername(1, APP_SNTP_SERVER_FALLBACK);
    esp_sntp_setservername(2, APP_SNTP_SERVER_TERTIARY);
    esp_sntp_init();

    s_started = true;
    ESP_LOGI(TAG, "SNTP 已启动: %s / %s / %s",
             APP_SNTP_SERVER_PRIMARY, APP_SNTP_SERVER_FALLBACK, APP_SNTP_SERVER_TERTIARY);
    return ESP_OK;
}

bool time_sync_is_valid(void)
{
    /* 用时间戳判断而不依赖 SNTP 状态机，逻辑简单且不受实现变化影响 */
    return time(NULL) >= (time_t)APP_TIME_MIN_VALID_EPOCH;
}

esp_err_t time_sync_wait_valid(uint32_t timeout_ms)
{
    if (time_sync_is_valid()) {
        return ESP_OK;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!time_sync_is_valid()) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGW(TAG, "等待对时超时 (%u ms)", (unsigned)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    ESP_LOGI(TAG, "对时完成: %04d-%02d-%02d %02d:%02d:%02d",
             local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
             local_time.tm_hour, local_time.tm_min, local_time.tm_sec);
    return ESP_OK;
}
