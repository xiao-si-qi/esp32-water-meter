/**
 * @file mqtt_manager.c
 * @brief switchyard 设备协议实现（水表）。
 *
 * 与圆屏（orbis-panel）使用同一套协议：
 *
 *   上行  switchyard/<id>/state         每 60s + 状态变化时（retained）
 *   上行  switchyard/<id>/availability  online + LWT offline
 *   上行  switchyard/<id>/meta          能力声明（retained）
 *   上行  switchyard/<id>/event         事件 JSON（retained，最近一次）
 *   下行  switchyard/<id>/cmd           带 seq 的命令（去重）
 *
 * 对内的接口（mqtt_publish_*）保持不变，因此 telemetry.c / valve.c / web_server.c
 * 的调用点无需修改；原先十几条分散主题改为一条聚合 JSON，由 switchyard 解析并
 * 生成 HA 实体。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "app_config.h"
#include "app_state.h"
#include "device_settings.h"
#include "leds.h"
#include "log_buffer.h"
#include "mqtt_manager.h"
#include "mqtt_settings.h"
#include "ota_remote.h"
#include "time_sync.h"
#include "valve.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static SemaphoreHandle_t s_connect_request = NULL;

/* 配置对象与证书必须放静态存储：MQTT 客户端按引用使用其中指针 */
static mqtt_settings_t s_settings;
static esp_mqtt_client_config_t s_client_config;
static char s_client_id[64];

/* 最近一次命令序号，用于过滤重连后重复投递的 retained 命令 */
static int32_t s_last_cmd_seq = 0;

static void housekeeping(void);

/* 变更标记：多个量在同一次采集里连续上报时只发一条状态 */
static volatile bool s_state_dirty = false;
static volatile int64_t s_last_state_ms = 0;

/* ============================ 状态上报 ============================ */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void publish_raw(const char *topic, const char *payload, int qos, bool retain)
{
    if (s_client == NULL || !app_state_mqtt_connected()) {
        return;
    }
    const int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    if (msg_id >= 0) {
        leds_notify_mqtt_activity();
    } else {
        ESP_LOGW(TAG, "发布失败: %s", topic);
    }
}

/** 把所有状态打成一个 JSON 发到 state 主题（retained）。 */
static void publish_state(void)
{
    app_state_t st;
    app_state_snapshot(&st);

    const int64_t ts = time_sync_is_valid() ? (int64_t)time(NULL) : 0;

    char json[512];
    snprintf(json, sizeof(json),
             "{\"id\":\"%s\",\"type\":\"%s\",\"fw\":\"%s\""
             ",\"rssi\":%d,\"ip\":\"%s\""
             ",\"battery\":%d,\"battery_v\":%.3f"
             ",\"temp\":%.2f,\"hum\":%.2f"
             ",\"valve\":%s,\"leak\":%s,\"comm_fault\":%s"
             ",\"water\":%.2f,\"water_unit\":\"m3\""
             ",\"ts\":%lld,\"uptime\":%lld}",
             APP_SY_DEVICE_ID, APP_SY_TYPE, esp_app_get_description()->version,
             st.rssi_dbm, st.ip_addr,
             st.battery_percent, st.battery_voltage_v,
             st.temperature_c, st.humidity_pct,
             st.valve_open ? "true" : "false",
             st.leak_detected ? "true" : "false",
             st.comm_fail ? "true" : "false",
             st.volume_m3,
             (long long)ts,
             (long long)(esp_timer_get_time() / 1000000));

    publish_raw(APP_SY_TOPIC_STATE, json, 1, true);
    s_last_state_ms = now_ms();
    s_state_dirty = false;
}

/** 能力声明：告诉 switchyard 这台设备会什么（决定它生成哪些 HA 实体/按钮）。 */
static void publish_meta(void)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"type\":\"%s\",\"caps\":[%s],\"fw\":\"%s\""
             ",\"name\":\"%s\",\"model\":\"%s\"}",
             APP_SY_TYPE, APP_SY_CAPS, esp_app_get_description()->version,
             device_settings_name(), APP_DEVICE_MODEL);
    publish_raw(APP_SY_TOPIC_META, json, 1, true);
}

/** 事件：retained，HA 重启后仍能看到"最近事件"。 */
static void publish_event_json(const char *event)
{
    if (event == NULL) {
        return;
    }
    char json[160];
    if (strchr(event, '{') != NULL) {
        /* 已经是 JSON 就原样发（ota_progress 之类） */
        snprintf(json, sizeof(json), "%s", event);
    } else {
        snprintf(json, sizeof(json), "{\"ev\":\"%s\"}", event);
    }
    publish_raw(APP_SY_TOPIC_EVENT, json, 1, true);
}

/* ============================ 命令下行 ============================ */

static int32_t load_cmd_seq(void)
{
    nvs_handle_t handle;
    int32_t seq = 0;
    if (nvs_open(APP_SY_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_i32(handle, "cmd_seq", &seq);
        nvs_close(handle);
    }
    return seq;
}

static void save_cmd_seq(int32_t seq)
{
    nvs_handle_t handle;
    if (nvs_open(APP_SY_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "cmd_seq", seq);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

/** 把 JSON 值转成开关：支持 true/false、\"on\"/\"off\"、1/0。 */
static bool json_to_bool(const cJSON *item, bool *out)
{
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsString(item)) {
        const char *v = item->valuestring;
        if (strcasecmp(v, "on") == 0 || strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0) {
            *out = true;
            return true;
        }
        if (strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0) {
            *out = false;
            return true;
        }
    }
    if (cJSON_IsNumber(item)) {
        *out = item->valueint != 0;
        return true;
    }
    return false;
}

static void handle_cmd(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (root == NULL) {
        ESP_LOGW(TAG, "命令不是合法 JSON，忽略");
        return;
    }

    leds_notify_mqtt_activity();

    const cJSON *jseq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    if (cJSON_IsNumber(jseq)) {
        const int32_t seq = jseq->valueint;
        if (seq != 0 && seq <= s_last_cmd_seq) {
            ESP_LOGI(TAG, "命令 seq=%ld 已执行过（last=%ld），跳过", (long)seq, (long)s_last_cmd_seq);
            cJSON_Delete(root);
            return;
        }
        s_last_cmd_seq = seq;
        save_cmd_seq(seq);
    }

    const cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(jcmd)) {
        cJSON_Delete(root);
        return;
    }
    const char *cmd = jcmd->valuestring;
    ESP_LOGI(TAG, "收到命令: %s", cmd);

    if (strcmp(cmd, "reboot") == 0) {
        ESP_LOGW(TAG, "命令重启");
        publish_event_json("cmd_reboot");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (strcmp(cmd, "state") == 0) {
        publish_meta();
        publish_state();
    } else if (strcmp(cmd, "ota") == 0) {
        const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(root, "url");
        const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
        if (cJSON_IsString(jurl)) {
            ota_remote_start(jurl->valuestring, cJSON_IsString(jver) ? jver->valuestring : NULL);
        } else {
            ota_remote_check_manifest();
        }
    } else if (strcmp(cmd, "valve") == 0) {
        const cJSON *jopen = cJSON_GetObjectItemCaseSensitive(root, "open");
        bool open = false;
        if (json_to_bool(jopen, &open)) {
            valve_request(VALVE_SOURCE_MQTT, open);
        }
    } else if (strcmp(cmd, "log") == 0) {
        /* 按需抓日志：非 retained，发一次就完，不进 HA 历史 */
        const cJSON *jlines = cJSON_GetObjectItemCaseSensitive(root, "lines");
        int lines = cJSON_IsNumber(jlines) ? jlines->valueint : 40;
        if (lines <= 0 || lines > 40) {
            lines = 40;
        }
        char *dump = malloc(4096);
        if (dump != NULL) {
            const size_t n = log_buffer_dump(dump, 4096, lines);
            if (n > 0) {
                publish_raw(APP_SY_TOPIC_LOG, dump, 0, false);
                ESP_LOGI(TAG, "已发送 %u 字节日志", (unsigned)n);
            }
            free(dump);
        }
    } else if (strcmp(cmd, "set") == 0) {
        const cJSON *jkey = cJSON_GetObjectItemCaseSensitive(root, "key");
        const cJSON *jval = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (cJSON_IsString(jkey) && strcmp(jkey->valuestring, "valve") == 0) {
            bool open = false;
            if (json_to_bool(jval, &open)) {
                valve_request(VALVE_SOURCE_MQTT, open);
            }
        }
    } else {
        ESP_LOGW(TAG, "未知命令: %s", cmd);
    }

    cJSON_Delete(root);
}

/* ============================ 连接管理 ============================ */

static void log_transport_error(const esp_mqtt_event_handle_t event)
{
    const esp_mqtt_error_codes_t *error = event->error_handle;
    if (error == NULL) {
        ESP_LOGW(TAG, "MQTT 错误事件（无详细信息）");
        return;
    }
    ESP_LOGE(TAG, "MQTT 错误: type=%d, tls=0x%X, sock_errno=%d", (int)error->error_type,
             error->esp_tls_last_esp_err, (int)error->esp_transport_sock_errno);
}

static void mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT 已连接 (%s)，设备 %s",
                 mqtt_settings_is_tls(&s_settings) ? "TLS" : "明文", APP_SY_DEVICE_ID);
        app_state_set_mqtt(true);
        leds_notify_mqtt_activity();

        publish_raw(APP_SY_TOPIC_AVAILABILITY, "online", 1, true);
        publish_meta();
        publish_state();
        esp_mqtt_client_subscribe(s_client, APP_SY_TOPIC_CMD, 0);

        /* 连上后补发"最近事件"，避免 HA 停留在上一次会话 */
        {
            app_state_t st;
            app_state_snapshot(&st);
            if (st.last_event[0] != '\0') {
                publish_event_json(st.last_event);
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT 断开，等待自动重连");
        app_state_set_mqtt(false);
        break;

    case MQTT_EVENT_DATA:
        if (event->topic_len == (int)strlen(APP_SY_TOPIC_CMD) &&
            strncmp(event->topic, APP_SY_TOPIC_CMD, (size_t)event->topic_len) == 0) {
            handle_cmd(event->data, event->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        log_transport_error(event);
        break;

    default:
        break;
    }
}

static void configure_tls(esp_mqtt_client_config_t *config)
{
    if (!mqtt_settings_is_tls(&s_settings)) {
        return;
    }
    switch (s_settings.tls_verify) {
    case MQTT_TLS_NO_VERIFY:
        config->broker.verification.skip_cert_common_name_check = true;
        ESP_LOGW(TAG, "TLS 证书校验已关闭，仅用于调试");
        break;
    case MQTT_TLS_VERIFY_CUSTOM_CA:
        config->broker.verification.certificate = s_settings.ca_cert;
        config->broker.verification.certificate_len = strlen(s_settings.ca_cert) + 1;
        break;
    case MQTT_TLS_VERIFY_BUNDLE:
    default:
        config->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        break;
    }
}

static esp_err_t mqtt_client_start(void)
{
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "读取 MAC 失败");

    snprintf(s_client_id, sizeof(s_client_id), "%s%02X%02X%02X%02X%02X%02X",
             s_settings.client_id_prefix, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    memset(&s_client_config, 0, sizeof(s_client_config));
    s_client_config.broker.address.uri = s_settings.uri;
    s_client_config.credentials.username = s_settings.username;
    s_client_config.credentials.client_id = s_client_id;
    s_client_config.credentials.authentication.password = s_settings.password;

    s_client_config.session.keepalive = APP_MQTT_KEEPALIVE_S;
    s_client_config.session.last_will.topic = APP_SY_TOPIC_AVAILABILITY;
    s_client_config.session.last_will.msg = "offline";
    s_client_config.session.last_will.msg_len = 7;
    s_client_config.session.last_will.qos = 1;
    s_client_config.session.last_will.retain = 1;

    /* 状态 JSON 不到 512 字节，收 1KB 就够；省 RAM 给 TLS */
    s_client_config.buffer.size = 2048;
    s_client_config.buffer.out_size = 1024;

    configure_tls(&s_client_config);

    s_client = esp_mqtt_client_init(&s_client_config);
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_NO_MEM, TAG, "创建 MQTT 客户端失败");

    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL),
                        TAG, "注册事件失败");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "启动客户端失败");

    ESP_LOGI(TAG, "MQTT 启动: %s（topic 前缀 %s）", s_settings.uri, APP_SY_TOPIC_PREFIX);
    return ESP_OK;
}

static void mqtt_client_stop_and_destroy(void)
{
    if (s_client == NULL) {
        return;
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    app_state_set_mqtt(false);
}

static void mqtt_supervisor_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (xSemaphoreTake(s_connect_request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        mqtt_settings_load(&s_settings);
        mqtt_client_stop_and_destroy();

        while (!app_state_wifi_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (mqtt_settings_is_tls(&s_settings) && !time_sync_is_valid()) {
            ESP_LOGI(TAG, "加密连接需先对时，等待 SNTP…");
            while (!time_sync_is_valid() && app_state_wifi_connected()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (!app_state_wifi_connected()) {
                continue;
            }
        }

        if (mqtt_client_start() != ESP_OK) {
            ESP_LOGE(TAG, "MQTT 启动失败");
        }
    }
}

/* ============================ 周期任务 ============================ */

/**
 * @brief 上报节拍 + 定时自查固件。
 *
 * 独立任务，避免把等待逻辑塞进 MQTT 事件回调（会阻塞协议栈）。
 */
static void mqtt_report_task(void *arg)
{
    (void)arg;
    int64_t last_ota_check = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        housekeeping();

        /* 错误摘要：有就发，但 1 分钟内只发一条 */
        static int64_t s_last_diag_ms = 0;
        const int64_t tick = now_ms();
        if (app_state_mqtt_connected() && (tick - s_last_diag_ms) >= APP_DIAG_MIN_INTERVAL_MS) {
            char diag[768];
            if (log_buffer_take_diag(diag, sizeof(diag))) {
                publish_raw(APP_SY_TOPIC_DIAG, diag, 1, true);
                s_last_diag_ms = tick;
            }
        }

        if (app_state_mqtt_connected() && !ota_remote_busy()) {
            const int64_t now = now_ms();
            if ((now - last_ota_check) >= (int64_t)APP_OTA_CHECK_INTERVAL_S * 1000) {
                last_ota_check = now;
                ota_remote_check_manifest();
            }
        }
    }
}

/* ============================ 对外接口 ============================ */

esp_err_t mqtt_manager_init(void)
{
    if (s_connect_request != NULL) {
        return ESP_OK;
    }
    s_connect_request = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_connect_request != NULL, ESP_ERR_NO_MEM, TAG, "创建信号量失败");

    log_buffer_init();
    s_last_cmd_seq = load_cmd_seq();

    const BaseType_t created = xTaskCreate(mqtt_supervisor_task, "mqtt_sup", TASK_STACK_MQTT_SUPERVISOR,
                                           NULL, TASK_PRIO_MQTT_SUPERVISOR, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "创建监督任务失败");

    const BaseType_t reporter = xTaskCreate(mqtt_report_task, "mqtt_rpt", 4096, NULL, 2, NULL);
    ESP_RETURN_ON_FALSE(reporter == pdPASS, ESP_ERR_NO_MEM, TAG, "创建上报任务失败");
    return ESP_OK;
}

esp_err_t mqtt_manager_start(void)
{
    ESP_RETURN_ON_FALSE(s_connect_request != NULL, ESP_ERR_INVALID_STATE, TAG, "模块未初始化");
    xSemaphoreGive(s_connect_request);
    return ESP_OK;
}

esp_err_t mqtt_manager_restart(void)
{
    return mqtt_manager_start();
}

/* ---- 以下为 telemetry / valve 使用的上报接口（保持原有签名）---- */

void mqtt_manager_on_valve_state(bool is_open, bool is_moving)
{
    (void)is_open;
    (void)is_moving;
    s_state_dirty = true;
}

void mqtt_manager_on_valve_event(const char *event, bool fault_active)
{
    (void)fault_active;   /* 故障状态在主 state 的 valve_fault 之外的字段里体现 */
    publish_event_json(event);
    s_state_dirty = true;
}

void mqtt_publish_event(const char *event)
{
    publish_event_json(event);
}

void mqtt_publish_valve_fault(bool fault)
{
    (void)fault;
    s_state_dirty = true;
}

void mqtt_publish_comm_fault(bool fault)
{
    (void)fault;
    s_state_dirty = true;
}

void mqtt_publish_leak(bool detected)
{
    (void)detected;
    s_state_dirty = true;
}

void mqtt_publish_water(float volume_m3)
{
    (void)volume_m3;
    s_state_dirty = true;
}

void mqtt_publish_valve_state(bool is_open)
{
    (void)is_open;
    s_state_dirty = true;
}

void mqtt_publish_battery(float voltage_v, int percent)
{
    (void)voltage_v;
    (void)percent;
    s_state_dirty = true;
}

void mqtt_publish_climate(float temperature_c, float humidity_pct)
{
    (void)temperature_c;
    (void)humidity_pct;
    s_state_dirty = true;
}

void mqtt_publish_diagnostics(const char *ip_addr, int rssi_dbm)
{
    (void)ip_addr;
    (void)rssi_dbm;
    s_state_dirty = true;
}

void mqtt_publish_all(void)
{
    publish_state();
}

/**
 * @brief 合并上报的节拍任务。
 *
 * telemetry 一轮会连续调用 4~5 个 mqtt_publish_*，如果每个都发一条 retained
 * 消息，broker 和 HA 都会被刷。这里改成"打标记 + 每 2 秒最多发一次"，
 * 另外每 APP_SY_STATE_PERIOD_S 秒强制发一次（设备长时间无变化时保活）。
 */
static void housekeeping(void)
{
    const int64_t now = now_ms();
    if (s_state_dirty && (now - s_last_state_ms) >= 2000) {
        publish_state();
    }
    static int64_t s_last_quiet_pub = 0;
    if ((now - s_last_quiet_pub) >= (int64_t)APP_SY_STATE_PERIOD_S * 1000) {
        s_last_quiet_pub = now;
        if (!s_state_dirty) {
            publish_state();
        }
    }
}
