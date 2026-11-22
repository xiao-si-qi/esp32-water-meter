/**
 * @file device_settings.c
 * @brief 中文显示名的 NVS 存储。
 */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "app_config.h"
#include "device_settings.h"

static const char *TAG = "devcfg";

#define NVS_NAMESPACE "device"
#define NVS_KEY_NAME  "name"

static char s_name[DEVICE_NAME_MAX] = APP_DEVICE_NAME;

const char *device_settings_name(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(s_name);
        char buf[DEVICE_NAME_MAX];
        if (nvs_get_str(handle, NVS_KEY_NAME, buf, &len) == ESP_OK && buf[0] != '\0') {
            strlcpy(s_name, buf, sizeof(s_name));
        }
        nvs_close(handle);
    }
    return s_name;
}

esp_err_t device_settings_set_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, NVS_KEY_NAME, name);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        strlcpy(s_name, name, sizeof(s_name));
        ESP_LOGI(TAG, "中文名已保存: %s", s_name);
    }
    return err;
}
