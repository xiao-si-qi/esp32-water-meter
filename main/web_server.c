#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include "app_config.h"
#include "app_state.h"
#include "auth_settings.h"
#include "device_settings.h"
#include "battery.h"
#include "mqtt_manager.h"
#include "mqtt_settings.h"
#include "valve.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "wifi_settings.h"

static const char *TAG = "httpd";

/* 由 EMBED_FILES 生成的内嵌页面符号 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server = NULL;

/* ============================ 工具函数 ============================ */

static esp_err_t web_send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t web_send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    return web_send_json(req, root);
}

static esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return web_send_json(req, root);
}

/** @brief 读取完整请求体，超长直接拒绝（避免爆栈）。 */
static esp_err_t web_read_body(httpd_req_t *req, char *buffer, size_t capacity)
{
    if (req->content_len == 0) {
        buffer[0] = '\0';
        return ESP_OK;
    }
    if (req->content_len >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < (size_t)req->content_len) {
        const int ret = httpd_req_recv(req, buffer + received,
                                       (size_t)req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

/* ============================ 登录认证 ============================ */

#define AUTH_REALM "ESP32 Water Meter"

/** @brief 定时攻击无关的比较，避免逐字节短路泄露信息 */
static bool constant_time_equal(const char *lhs, const char *rhs)
{
    const size_t lhs_len = strlen(lhs);
    const size_t rhs_len = strlen(rhs);
    if (lhs_len != rhs_len) {
        return false;
    }

    unsigned char diff = 0;
    for (size_t i = 0; i < lhs_len; i++) {
        diff |= (unsigned char)(lhs[i] ^ rhs[i]);
    }
    return diff == 0;
}

/**
 * @brief 校验请求头里的 Basic 凭据。
 *
 * 手写解析而不是用 httpd 内置的 uri->auth：内置支持需要在 menuconfig 里
 * 打开对应选项，且凭据写死在 handler 结构体里，无法运行时修改。
 */
static bool web_auth_ok(httpd_req_t *req)
{
    char header[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return false;
    }
    if (strncasecmp(header, "Basic ", 6) != 0) {
        return false;
    }

    unsigned char decoded[160];
    size_t decoded_len = 0;
    const int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                          (const unsigned char *)header + 6,
                                          strlen(header + 6));
    if (ret != 0 || decoded_len == 0) {
        return false;
    }
    decoded[decoded_len] = '\0';

    auth_settings_t settings;
    auth_settings_load(&settings);

    /* 期望值形如 "user:pass"，用 memcpy 拼接避免格式化截断告警 */
    char expected[160];
    const size_t user_len = strlen(settings.username);
    const size_t pass_len = strlen(settings.password);
    if (user_len + pass_len + 2 > sizeof(expected)) {
        return false;
    }
    memcpy(expected, settings.username, user_len);
    expected[user_len] = ':';
    memcpy(expected + user_len + 1, settings.password, pass_len);
    expected[user_len + 1 + pass_len] = '\0';

    return constant_time_equal((const char *)decoded, expected);
}

static esp_err_t web_send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"" AUTH_REALM "\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "需要登录");
}

/** @brief 每个 handler 开头统一调用 */
#define REQUIRE_AUTH(request)                    \
    do {                                         \
        if (!web_auth_ok(request)) {             \
            return web_send_unauthorized(request); \
        }                                        \
    } while (0)

/* ============================ 页面与状态 ============================ */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    app_state_t state;
    app_state_snapshot(&state);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "volume_m3", state.volume_m3);
    cJSON_AddBoolToObject(root, "valve_open", state.valve_open);
    cJSON_AddBoolToObject(root, "valve_moving", state.valve_moving);
    cJSON_AddBoolToObject(root, "comm_fail", state.comm_fail);
    cJSON_AddBoolToObject(root, "valve_fault", state.valve_fault);
    cJSON_AddBoolToObject(root, "leak_detected", state.leak_detected);
    cJSON_AddStringToObject(root, "last_event", state.last_event);
    cJSON_AddNumberToObject(root, "battery_v", state.battery_voltage_v);
    cJSON_AddNumberToObject(root, "battery_pct", state.battery_percent);
    cJSON_AddNumberToObject(root, "temperature", state.temperature_c);
    cJSON_AddNumberToObject(root, "humidity", state.humidity_pct);
    cJSON_AddNumberToObject(root, "rssi", state.rssi_dbm);
    cJSON_AddStringToObject(root, "ip", state.ip_addr);
    cJSON_AddBoolToObject(root, "wifi", state.wifi_connected);
    cJSON_AddBoolToObject(root, "mqtt", state.mqtt_connected);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));

    /* 固件信息：版本号来自 CMake 的 PROJECT_VER，写在 app 镜像描述里 */
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (app != NULL) {
        cJSON_AddStringToObject(root, "version", app->version);
        cJSON_AddStringToObject(root, "build_time", app->time);
    }
    cJSON_AddStringToObject(root, "partition", running ? running->label : "unknown");

    return web_send_json(req, root);
}

/* ============================ 阀门控制 ============================ */

static esp_err_t valve_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char body[128];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "请求体为空或过长");
    }

    /* 同时接受 {"state":"on"} 与纯文本 on —— 方便 curl 直接调 */
    char action[16] = {0};
    cJSON *root = cJSON_Parse(body);
    if (root != NULL) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            strncpy(action, item->valuestring, sizeof(action) - 1);
        }
        cJSON_Delete(root);
    } else {
        strncpy(action, body, sizeof(action) - 1);
    }

    bool target_open;
    if (strcasecmp(action, "on") == 0 || strcasecmp(action, "true") == 0 ||
        strcmp(action, "1") == 0) {
        target_open = true;
    } else if (strcasecmp(action, "off") == 0 || strcasecmp(action, "false") == 0 ||
               strcmp(action, "0") == 0) {
        target_open = false;
    } else {
        return web_send_error(req, "400 Bad Request", "state 只能是 on 或 off");
    }

    const esp_err_t err = valve_request(VALVE_SOURCE_WEB, target_open);
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "阀门模块未就绪");
    }

    return web_send_ok(req);
}

/* ============================ MQTT 配置 ============================ */

/** GET /api/device/config：设备 ID（只读）+ 中文名。 */
static esp_err_t device_config_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", APP_DEVICE_ID);
    cJSON_AddStringToObject(root, "name", device_settings_name());
    cJSON_AddStringToObject(root, "type", APP_SY_TYPE);
    return web_send_json(req, root);
}

/** POST /api/device/config：{"name":"..."} */
static esp_err_t device_config_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char body[256];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "请求体为空或过长");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return web_send_error(req, "400 Bad Request", "JSON 解析失败");
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name) || name->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return web_send_error(req, "400 Bad Request", "name 不能为空");
    }
    const esp_err_t err = device_settings_set_name(name->valuestring);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return web_send_error(req, "500 Internal Server Error", "保存失败");
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "name", device_settings_name());
    cJSON_Delete(root);
    return web_send_json(req, resp);
}

static esp_err_t mqtt_config_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    mqtt_settings_t settings;
    mqtt_settings_load(&settings);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "uri", settings.uri);
    cJSON_AddStringToObject(root, "username", settings.username);
    cJSON_AddBoolToObject(root, "tls", mqtt_settings_is_tls(&settings));
    cJSON_AddNumberToObject(root, "tls_verify", (int)settings.tls_verify);
    /* 密码不下发，只告知是否已设置 */
    cJSON_AddBoolToObject(root, "password_set", settings.password[0] != '\0');

    return web_send_json(req, root);
}

static esp_err_t mqtt_config_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char body[512];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "请求体为空或过长");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return web_send_error(req, "400 Bad Request", "JSON 解析失败");
    }

    mqtt_settings_t settings;
    mqtt_settings_load(&settings);

    const cJSON *uri = cJSON_GetObjectItemCaseSensitive(root, "uri");
    const cJSON *user = cJSON_GetObjectItemCaseSensitive(root, "username");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *verify = cJSON_GetObjectItemCaseSensitive(root, "tls_verify");

    if (cJSON_IsString(uri) && uri->valuestring[0] != '\0') {
        strncpy(settings.uri, uri->valuestring, sizeof(settings.uri) - 1);
    }
    if (cJSON_IsString(user)) {
        strncpy(settings.username, user->valuestring, sizeof(settings.username) - 1);
    }
    /* 密码留空表示"不修改"，避免页面回显明文密码 */
    if (cJSON_IsString(pass) && pass->valuestring[0] != '\0') {
        strncpy(settings.password, pass->valuestring, sizeof(settings.password) - 1);
    }
    if (cJSON_IsNumber(verify) && verify->valueint >= (int)MQTT_TLS_VERIFY_BUNDLE &&
        verify->valueint <= (int)MQTT_TLS_NO_VERIFY) {
        settings.tls_verify = (mqtt_tls_verify_t)verify->valueint;
    }
    cJSON_Delete(root);

    if (strncmp(settings.uri, "mqtt://", 7) != 0 &&
        strncmp(settings.uri, "mqtts://", 8) != 0) {
        return web_send_error(req, "400 Bad Request",
                              "URI 必须以 mqtt:// 或 mqtts:// 开头");
    }

    const esp_err_t err = mqtt_settings_save(&settings);
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "写入 NVS 失败");
    }

    mqtt_manager_restart();
    ESP_LOGI(TAG, "网页已更新 MQTT 配置并触发重连");
    return web_send_ok(req);
}

/* ============================ Wi-Fi 配置 ============================ */

static esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char body[256];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "请求体为空或过长");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return web_send_error(req, "400 Bad Request", "JSON 解析失败");
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "password");

    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return web_send_error(req, "400 Bad Request", "SSID 不能为空");
    }

    wifi_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    strncpy(settings.ssid, ssid->valuestring, sizeof(settings.ssid) - 1);
    if (cJSON_IsString(pass)) {
        strncpy(settings.password, pass->valuestring, sizeof(settings.password) - 1);
    }
    cJSON_Delete(root);

    const esp_err_t err = wifi_settings_save(&settings);
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "写入 NVS 失败");
    }

    /* 立即用新凭据重连 */
    wifi_manager_apply_sta_config();
    ESP_LOGI(TAG, "网页已更新 Wi-Fi 凭据，正在重连");
    return web_send_ok(req);
}

/* ============================ 重启 ============================ */

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    const esp_err_t err = web_send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(300));   /* 留时间把响应发出去 */
    esp_restart();
    return err;
}

/* ============================ 电池诊断 ============================ */

/**
 * @brief 立即做一次电池采样，返回全部中间值。
 *
 * 用途：与万用表读数在同一时刻对照，判断是"读数偏"还是"读数漂"。
 * 每调用一次会现场采样 10 个点（约 50 ms），不依赖 60 秒的遥测周期。
 */
static esp_err_t battery_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    battery_measurement_t m;
    if (battery_measure(&m) != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "电池采样失败");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "raw_median", m.raw_median);
    cJSON_AddNumberToObject(root, "raw_min", m.raw_min);
    cJSON_AddNumberToObject(root, "raw_max", m.raw_max);
    cJSON_AddNumberToObject(root, "raw_count", m.raw_count);
    cJSON_AddNumberToObject(root, "raw_spread", m.raw_max - m.raw_min);
    cJSON_AddNumberToObject(root, "pin_mv", m.pin_mv);
    cJSON_AddNumberToObject(root, "voltage_v", m.voltage_v);
    cJSON_AddNumberToObject(root, "percent", m.percent);
    return web_send_json(req, root);
}

/* ============================ 登录凭据 ============================ */

static esp_err_t auth_config_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    auth_settings_t settings;
    auth_settings_load(&settings);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    /* 只回显用户名，密码不下发 */
    cJSON_AddStringToObject(root, "username", settings.username);
    return web_send_json(req, root);
}

static esp_err_t auth_config_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char body[256];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "请求体为空或过长");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return web_send_error(req, "400 Bad Request", "JSON 解析失败");
    }

    const cJSON *user = cJSON_GetObjectItemCaseSensitive(root, "username");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "password");

    auth_settings_t settings;
    auth_settings_load(&settings);

    if (cJSON_IsString(user)) {
        if (user->valuestring[0] == '\0') {
            cJSON_Delete(root);
            return web_send_error(req, "400 Bad Request", "用户名不能为空");
        }
        strncpy(settings.username, user->valuestring, sizeof(settings.username) - 1);
        settings.username[sizeof(settings.username) - 1] = '\0';
    }

    if (cJSON_IsString(pass)) {
        if (pass->valuestring[0] == '\0') {
            cJSON_Delete(root);
            return web_send_error(req, "400 Bad Request", "密码不能为空");
        }
        strncpy(settings.password, pass->valuestring, sizeof(settings.password) - 1);
        settings.password[sizeof(settings.password) - 1] = '\0';
    }
    cJSON_Delete(root);

    const esp_err_t err = auth_settings_save(&settings);
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "写入 NVS 失败");
    }

    ESP_LOGI(TAG, "网页登录凭据已修改，用户名: %s", settings.username);
    return web_send_ok(req);
}

/* ============================ 固件更新 ============================ */

#define OTA_RECV_BUFFER_SIZE 2048

static esp_err_t ota_info_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "running", running ? running->label : "unknown");
    cJSON_AddStringToObject(root, "target", target ? target->label : "unknown");
    cJSON_AddNumberToObject(root, "slot_size", target ? (double)target->size : 0);

    const esp_app_desc_t *app = esp_app_get_description();
    if (app != NULL) {
        cJSON_AddStringToObject(root, "version", app->version);
        cJSON_AddStringToObject(root, "build_time", app->time);
    }

    return web_send_json(req, root);
}

/**
 * @brief 接收浏览器上传的固件并写入备用 OTA 分区。
 *
 * 写成功后才调用 esp_ota_set_boot_partition 切换启动分区，
 * 因此中途断开或校验失败都不会让设备变砖（仍从原分区启动）。
 */
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (req->content_len <= 0) {
        return web_send_error(req, "400 Bad Request", "未收到固件数据");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return web_send_error(req, "500 Internal Server Error", "没有可用的 OTA 分区");
    }
    if ((size_t)req->content_len > target->size) {
        ESP_LOGE(TAG, "固件过大: %d > %u", req->content_len, (unsigned)target->size);
        return web_send_error(req, "400 Bad Request", "固件超过分区容量");
    }

    ESP_LOGI(TAG, "开始接收固件: %d 字节 -> 分区 %s", req->content_len, target->label);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
        return web_send_error(req, "500 Internal Server Error", "初始化 OTA 失败");
    }

    char *buffer = malloc(OTA_RECV_BUFFER_SIZE);
    if (buffer == NULL) {
        esp_ota_abort(ota_handle);
        return web_send_error(req, "500 Internal Server Error", "内存不足");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        const int chunk = (remaining < OTA_RECV_BUFFER_SIZE) ? remaining : OTA_RECV_BUFFER_SIZE;
        const int received = httpd_req_recv(req, buffer, chunk);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "接收中断，剩余 %d 字节", remaining);
            free(buffer);
            esp_ota_abort(ota_handle);
            return web_send_error(req, "400 Bad Request", "传输中断");
        }

        err = esp_ota_write(ota_handle, buffer, (size_t)received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写入失败: %s", esp_err_to_name(err));
            free(buffer);
            esp_ota_abort(ota_handle);
            return web_send_error(req, "500 Internal Server Error", "写入 flash 失败");
        }
        remaining -= received;
    }
    free(buffer);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        /* 镜像不合法（不是本芯片的固件、CRC 错等），原固件仍可正常启动 */
        ESP_LOGE(TAG, "固件校验失败: %s", esp_err_to_name(err));
        return web_send_error(req, "400 Bad Request", "固件校验失败，可能不是本设备的固件");
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置启动分区失败: %s", esp_err_to_name(err));
        return web_send_error(req, "500 Internal Server Error", "切换启动分区失败");
    }

    ESP_LOGI(TAG, "固件写入完成，即将从 %s 重启", target->label);
    web_send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* ============================ 服务器装配 ============================ */

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;          /* cJSON + 请求体缓冲需要额外栈 */

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "启动 HTTP 服务器失败");

    const httpd_uri_t routes[] = {
        {.uri = "/",                .method = HTTP_GET,  .handler = root_get_handler},
        {.uri = "/api/status",      .method = HTTP_GET,  .handler = status_get_handler},
        {.uri = "/api/valve",       .method = HTTP_POST, .handler = valve_post_handler},
        {.uri = "/api/auth/config", .method = HTTP_GET,  .handler = auth_config_get_handler},
        {.uri = "/api/auth/config", .method = HTTP_POST, .handler = auth_config_post_handler},
        {.uri = "/api/device/config", .method = HTTP_GET, .handler = device_config_get_handler},
{.uri = "/api/device/config", .method = HTTP_POST, .handler = device_config_post_handler},
{.uri = "/api/mqtt/config", .method = HTTP_GET,  .handler = mqtt_config_get_handler},
        {.uri = "/api/mqtt/config", .method = HTTP_POST, .handler = mqtt_config_post_handler},
        {.uri = "/api/wifi/config", .method = HTTP_POST, .handler = wifi_config_post_handler},
        {.uri = "/api/battery",     .method = HTTP_GET,  .handler = battery_get_handler},
        {.uri = "/api/ota",         .method = HTTP_GET,  .handler = ota_info_get_handler},
        {.uri = "/api/ota",         .method = HTTP_POST, .handler = ota_post_handler},
        {.uri = "/api/reboot",      .method = HTTP_POST, .handler = reboot_post_handler},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG,
                            "注册路由失败: %s", routes[i].uri);
    }

    ESP_LOGI(TAG, "网页服务已启动，端口 %d（%u 条路由）", config.server_port,
             (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server == NULL) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
}
