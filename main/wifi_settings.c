#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_settings.h"

static const char *TAG = "wifi_cfg";

#define NVS_NAMESPACE_WIFI  "wifi_cfg"
#define KEY_SSID            "ssid"
#define KEY_PASS            "pass"

esp_err_t wifi_settings_load(wifi_settings_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");

    memset(out, 0, sizeof(*out));
    snprintf(out->ssid, sizeof(out->ssid), "%s", APP_WIFI_SSID);
    snprintf(out->password, sizeof(out->password), "%s", APP_WIFI_PASSWORD);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGD(TAG, "无 NVS 覆盖，使用编译期默认凭据");
        return ESP_ERR_NOT_FOUND;
    }

    size_t length = sizeof(out->ssid);
    if (nvs_get_str(handle, KEY_SSID, out->ssid, &length) != ESP_OK) {
        snprintf(out->ssid, sizeof(out->ssid), "%s", APP_WIFI_SSID);
    }
    length = sizeof(out->password);
    if (nvs_get_str(handle, KEY_PASS, out->password, &length) != ESP_OK) {
        snprintf(out->password, sizeof(out->password), "%s", APP_WIFI_PASSWORD);
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "已载入 NVS 配置，SSID: %s", out->ssid);
    return ESP_OK;
}

esp_err_t wifi_settings_save(const wifi_settings_t *settings)
{
    ESP_RETURN_ON_FALSE(settings != NULL, ESP_ERR_INVALID_ARG, TAG, "参数为空");
    ESP_RETURN_ON_FALSE(settings->ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "SSID 不能为空");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &handle),
                        TAG, "打开 NVS 失败");

    esp_err_t err = nvs_set_str(handle, KEY_SSID, settings->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_PASS, settings->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi 凭据已保存，SSID: %s", settings->ssid);
    } else {
        ESP_LOGE(TAG, "保存 Wi-Fi 凭据失败: %s", esp_err_to_name(err));
    }
    return err;
}
