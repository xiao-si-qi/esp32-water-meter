#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "mqtt_settings.h"

static const char *TAG = "mqtt_cfg";

#define NVS_NAMESPACE_MQTT  "mqtt_cfg"
#define KEY_URI             "uri"
#define KEY_USER            "user"
#define KEY_PASS            "pass"
#define KEY_PREFIX          "prefix"
#define KEY_TLS_VERIFY      "tls_verify"
#define KEY_CA              "ca"

static void load_defaults(mqtt_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    snprintf(settings->uri, sizeof(settings->uri), "%s", APP_MQTT_BROKER_URI);
    snprintf(settings->username, sizeof(settings->username), "%s", APP_MQTT_USERNAME);
    snprintf(settings->password, sizeof(settings->password), "%s", APP_MQTT_PASSWORD);
    snprintf(settings->client_id_prefix, sizeof(settings->client_id_prefix), "%s",
             APP_MQTT_CLIENT_ID_PREFIX);
    settings->tls_verify = MQTT_TLS_VERIFY_BUNDLE;
    settings->ca_cert[0] = '\0';
}

static bool load_string(nvs_handle_t handle, const char *key, char *buffer, size_t capacity)
{
    size_t length = capacity;
    if (nvs_get_str(handle, key, buffer, &length) != ESP_OK) {
        return false;
    }
    buffer[capacity - 1] = '\0';
    return true;
}

esp_err_t mqtt_settings_load(mqtt_settings_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");

    load_defaults(out);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_MQTT, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGD(TAG, "无 NVS 覆盖，使用编译期默认配置");
        return ESP_ERR_NOT_FOUND;
    }

    if (!load_string(handle, KEY_URI, out->uri, sizeof(out->uri))) {
        snprintf(out->uri, sizeof(out->uri), "%s", APP_MQTT_BROKER_URI);
    }
    if (!load_string(handle, KEY_USER, out->username, sizeof(out->username))) {
        snprintf(out->username, sizeof(out->username), "%s", APP_MQTT_USERNAME);
    }
    if (!load_string(handle, KEY_PASS, out->password, sizeof(out->password))) {
        snprintf(out->password, sizeof(out->password), "%s", APP_MQTT_PASSWORD);
    }
    if (!load_string(handle, KEY_PREFIX, out->client_id_prefix, sizeof(out->client_id_prefix))) {
        snprintf(out->client_id_prefix, sizeof(out->client_id_prefix), "%s",
                 APP_MQTT_CLIENT_ID_PREFIX);
    }
    if (!load_string(handle, KEY_CA, out->ca_cert, sizeof(out->ca_cert))) {
        out->ca_cert[0] = '\0';
    }

    uint8_t verify_mode = 0;
    if (nvs_get_u8(handle, KEY_TLS_VERIFY, &verify_mode) == ESP_OK &&
        verify_mode <= (uint8_t)MQTT_TLS_NO_VERIFY) {
        out->tls_verify = (mqtt_tls_verify_t)verify_mode;
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "已载入 NVS 配置: %s (%s)", out->uri,
             mqtt_settings_is_tls(out) ? "TLS" : "明文");
    return ESP_OK;
}

esp_err_t mqtt_settings_save(const mqtt_settings_t *settings)
{
    ESP_RETURN_ON_FALSE(settings != NULL, ESP_ERR_INVALID_ARG, TAG, "参数为空");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE_MQTT, NVS_READWRITE, &handle),
                        TAG, "打开 NVS 失败");

    esp_err_t err = nvs_set_str(handle, KEY_URI, settings->uri);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_USER, settings->username);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_PASS, settings->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_PREFIX, settings->client_id_prefix);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_CA, settings->ca_cert);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_TLS_VERIFY, (uint8_t)settings->tls_verify);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT 配置已保存到 NVS: %s", settings->uri);
    } else {
        ESP_LOGE(TAG, "保存 MQTT 配置失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t mqtt_settings_reset(void)
{
    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(NVS_NAMESPACE_MQTT, NVS_READWRITE, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* 本来就没有覆盖，无需处理 */
    }
    ESP_RETURN_ON_ERROR(open_err, TAG, "打开 NVS 失败");

    const esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "已清除 MQTT 配置覆盖，恢复编译期默认值");
    }
    nvs_close(handle);
    return err;
}

bool mqtt_settings_is_tls(const mqtt_settings_t *settings)
{
    if (settings == NULL) {
        return false;
    }
    return strncmp(settings->uri, "mqtts://", 8) == 0 ||
           strncmp(settings->uri, "wss://", 6) == 0;
}
