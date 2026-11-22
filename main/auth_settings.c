#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "auth_settings.h"

static const char *TAG = "auth";

#define NVS_NAMESPACE_AUTH  "auth"
#define KEY_USER            "user"
#define KEY_PASS            "pass"

#define DEFAULT_USERNAME    "admin"
#define DEFAULT_PASSWORD    "admin"

esp_err_t auth_settings_load(auth_settings_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");

    memset(out, 0, sizeof(*out));
    snprintf(out->username, sizeof(out->username), "%s", DEFAULT_USERNAME);
    snprintf(out->password, sizeof(out->password), "%s", DEFAULT_PASSWORD);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_AUTH, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGD(TAG, "无 NVS 凭据，使用默认 admin/admin");
        return ESP_ERR_NOT_FOUND;
    }

    size_t length = sizeof(out->username);
    if (nvs_get_str(handle, KEY_USER, out->username, &length) != ESP_OK) {
        snprintf(out->username, sizeof(out->username), "%s", DEFAULT_USERNAME);
    }
    length = sizeof(out->password);
    if (nvs_get_str(handle, KEY_PASS, out->password, &length) != ESP_OK) {
        snprintf(out->password, sizeof(out->password), "%s", DEFAULT_PASSWORD);
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t auth_settings_save(const auth_settings_t *settings)
{
    ESP_RETURN_ON_FALSE(settings != NULL, ESP_ERR_INVALID_ARG, TAG, "参数为空");
    ESP_RETURN_ON_FALSE(settings->username[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "用户名不能为空");
    ESP_RETURN_ON_FALSE(settings->password[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "密码不能为空");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE_AUTH, NVS_READWRITE, &handle),
                        TAG, "打开 NVS 失败");

    esp_err_t err = nvs_set_str(handle, KEY_USER, settings->username);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_PASS, settings->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "网页凭据已更新，用户名: %s", settings->username);
    } else {
        ESP_LOGE(TAG, "保存网页凭据失败: %s", esp_err_to_name(err));
    }
    return err;
}
