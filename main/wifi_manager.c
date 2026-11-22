#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_state.h"
#include "mqtt_manager.h"
#include "time_sync.h"
#include "wifi_manager.h"
#include "wifi_settings.h"

static const char *TAG = "wifi";

static esp_timer_handle_t s_retry_timer = NULL;
static int s_retry_count = 0;

/**
 * @brief 重连定时器回调。
 *
 * 原实现直接在 WIFI_EVENT_STA_DISCONNECTED 回调里 vTaskDelay(最多 10 秒)，
 * 把系统事件循环任务整条堵死，期间所有事件（包括 MQTT、IP 事件）都无法分发。
 * 现在事件回调只负责“记数 + 启动一次性定时器”，立即返回。
 */
static void wifi_retry_timeout_cb(void *arg)
{
    (void)arg;

    if (s_retry_count >= APP_WIFI_RETRY_MAX) {
        ESP_LOGW(TAG, "连续失败 %d 次，重启 Wi-Fi 驱动", s_retry_count);
        s_retry_count = 0;
        esp_wifi_stop();
        esp_wifi_start();   /* 会触发 STA_START，随后自动重新 connect */
        return;
    }

    ESP_LOGI(TAG, "发起第 %d 次重连", s_retry_count);
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;

        app_state_set_wifi(false);
        app_state_set_mqtt(false);
        s_retry_count++;

        uint32_t backoff_ms = APP_WIFI_RETRY_BASE_MS;
        for (int i = 1; i < s_retry_count && backoff_ms < APP_WIFI_RETRY_CAP_MS; i++) {
            backoff_ms *= 2U;
        }
        if (backoff_ms > APP_WIFI_RETRY_CAP_MS) {
            backoff_ms = APP_WIFI_RETRY_CAP_MS;
        }

        ESP_LOGW(TAG, "STA 断开 (reason=%d, 第 %d 次), %u ms 后重连",
                 event->reason, s_retry_count, (unsigned)backoff_ms);

        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)backoff_ms * 1000ULL);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        char ip_text[16];

        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&event->ip_info.ip));

        s_retry_count = 0;
        app_state_set_wifi(true);
        app_state_set_ip(ip_text);
        ESP_LOGI(TAG, "STA 已连接，IP = %s", ip_text);

        /* 尽早对时：mqtts:// 建立 TLS 前必须拿到可信时间，否则证书校验必失败 */
        time_sync_start();

        /* MQTT 客户端内部自带重连，这里只需保证首次启动幂等 */
        mqtt_manager_start();
    }
}

static void fill_sta_config(const wifi_settings_t *settings, wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* 用 memcpy 而非 snprintf：目标字段是定长数组，按长度截断可避免
     * -Wformat-truncation 报警（IDF 把该类告警当作 error） */
    const size_t ssid_len = strlen(settings->ssid);
    const size_t ssid_copy = (ssid_len < sizeof(cfg->sta.ssid))
                                 ? ssid_len
                                 : sizeof(cfg->sta.ssid) - 1;
    memcpy(cfg->sta.ssid, settings->ssid, ssid_copy);

    const size_t pass_len = strlen(settings->password);
    const size_t pass_copy = (pass_len < sizeof(cfg->sta.password))
                                 ? pass_len
                                 : sizeof(cfg->sta.password) - 1;
    memcpy(cfg->sta.password, settings->password, pass_copy);

    /* 密码为空按开放网络处理，否则要求 WPA2 */
    cfg->sta.threshold.authmode = (settings->password[0] == '\0') ? WIFI_AUTH_OPEN
                                                                 : WIFI_AUTH_WPA2_PSK;
}

esp_err_t wifi_manager_apply_sta_config(void)
{
    wifi_settings_t settings;
    wifi_settings_load(&settings);

    wifi_config_t cfg;
    fill_sta_config(&settings, &cfg);

    /* 未连接时 disconnect 返回 ESP_ERR_WIFI_NOT_CONNECT，属正常情况 */
    esp_wifi_disconnect();

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg),
                        TAG, "应用新的 Wi-Fi 配置失败");

    s_retry_count = 0;   /* 重置退避，让新凭据立即尝试 */
    ESP_LOGI(TAG, "已切换到 SSID: %s，开始重新连接", settings.ssid);
    return esp_wifi_connect();
}

esp_err_t wifi_manager_start(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "擦除 NVS 失败");
        nvs_err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(nvs_err, TAG, "初始化 NVS 失败");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "初始化 netif 失败");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "创建默认事件循环失败");
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "初始化 Wi-Fi 失败");

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_retry_timeout_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "创建重连定时器失败");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "注册 Wi-Fi 事件失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "注册 IP 事件失败");

    wifi_settings_t settings;
    wifi_settings_load(&settings);

    wifi_config_t wifi_cfg;
    fill_sta_config(&settings, &wifi_cfg);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "设置 STA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "写入 Wi-Fi 配置失败");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "启动 Wi-Fi 失败");

    ESP_LOGI(TAG, "STA 启动，目标 SSID: %s", settings.ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_get_rssi(int *rssi_dbm)
{
    ESP_RETURN_ON_FALSE(rssi_dbm != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");

    int rssi = 0;
    const esp_err_t err = esp_wifi_sta_get_rssi(&rssi);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * RSSI 本身抖动较大（同一位置相邻两次可差 5~8 dBm），直接上报会让 HA 的
     * 信号曲线呈锯齿状。这里做指数滑动平均：首采样直接采用，之后按 0.4 权重更新，
     * 约 3 个周期（3 分钟）收敛到新环境，既能压抖动又不至于反应太慢。
     */
    static float s_rssi_ema = 0.0f;
    static bool s_rssi_valid = false;

    if (!s_rssi_valid) {
        s_rssi_ema = (float)rssi;
        s_rssi_valid = true;
    } else {
        s_rssi_ema = s_rssi_ema * 0.6f + (float)rssi * 0.4f;
    }

    const float rounded = (s_rssi_ema >= 0.0f) ? (s_rssi_ema + 0.5f) : (s_rssi_ema - 0.5f);
    *rssi_dbm = (int)rounded;
    return ESP_OK;
}
