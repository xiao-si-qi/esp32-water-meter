/**
 * @file ota_remote.c
 * @brief MQTT / manifest 触发的远程 OTA 实现。
 *
 * 安全要点（与网页 OTA 保持一致）：
 *   - 只写"非活动"分区，写满校验通过后才 esp_ota_set_boot_partition()；
 *   - 中途断电只会留下一个无效的待升级分区，当前固件仍可正常启动；
 *   - 下载失败不改动启动分区，设备继续跑旧版本。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "app_config.h"
#include "mqtt_manager.h"
#include "ota_remote.h"

static const char *TAG = "ota";

#define OTA_TASK_STACK   6144
#define OTA_TASK_PRIO    4
#define OTA_RECV_TIMEOUT_MS 15000

static volatile bool s_busy = false;
static char s_url[256];
static char s_version[32];

static void report(const char *name, int percent)
{
    char ev[96];
    if (percent >= 0) {
        snprintf(ev, sizeof(ev), "ota_%s_%d", name, percent);
    } else {
        snprintf(ev, sizeof(ev), "ota_%s", name);
    }
    ESP_LOGI(TAG, "%s", ev);
    mqtt_publish_event(ev);
}

/** 从 URL 里取文件名（含扩展名），失败返回 NULL。 */
static const char *url_filename(const char *url)
{
    const char *slash = strrchr(url, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : NULL;
}

static void ota_task(void *arg)
{
    (void)arg;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        report("ota_no_partition", -1);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "目标分区 %s @ 0x%lx, 固件 %s -> %s", target->label,
             (unsigned long)target->address, esp_app_get_description()->version, s_version);

    esp_http_client_config_t config = {
        .url = s_url,
        .timeout_ms = OTA_RECV_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        report("ota_http_init_failed", -1);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开连接失败: %s", esp_err_to_name(err));
        report("ota_connect_failed", -1);
        esp_http_client_cleanup(client);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    const int content_len = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d（检查 URL / public_url 配置）", status);
        report("ota_http_error", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "开始下载，大小 %d 字节", content_len);

    esp_ota_handle_t handle = 0;
    err = esp_ota_begin(target, content_len > 0 ? (size_t)content_len : OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
        report("ota_begin_failed", -1);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    char *buffer = malloc(4096);
    if (buffer == NULL) {
        report("ota_no_mem", -1);
        esp_ota_abort(handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    int total = 0;
    int last_pct = -1;
    bool ok = true;
    for (;;) {
        const int read = esp_http_client_read(client, buffer, 4096);
        if (read < 0) {
            ESP_LOGE(TAG, "下载中断（read=%d）", read);
            ok = false;
            break;
        }
        if (read == 0) {
            break;   /* 正常结束 */
        }
        err = esp_ota_write(handle, buffer, (size_t)read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写入失败: %s", esp_err_to_name(err));
            ok = false;
            break;
        }
        total += read;
        if (content_len > 0) {
            const int pct = (int)((int64_t)total * 100 / content_len);
            if (pct / 10 != last_pct / 10) {
                last_pct = pct;
                report("ota_progress", pct);
            }
        }
        /* 让出 CPU，避免长时间占用看门狗 */
        vTaskDelay(1);
    }
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok) {
        esp_ota_abort(handle);
        report("ota_failed", -1);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end 失败（镜像无效？）: %s", esp_err_to_name(err));
        report("ota_invalid_image", -1);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切换启动分区失败: %s", esp_err_to_name(err));
        report("ota_set_boot_failed", -1);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "固件已写入 %d 字节，1 秒后重启到 %s", total, target->label);
    report("ota_done", -1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    vTaskDelete(NULL);
}

esp_err_t ota_remote_start(const char *url, const char *version)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_busy) {
        ESP_LOGW(TAG, "已有升级任务在进行，忽略本次请求");
        return ESP_ERR_INVALID_STATE;
    }
    if (url_filename(url) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_url, url, sizeof(s_url));
    strlcpy(s_version, (version != NULL) ? version : "?", sizeof(s_version));
    s_busy = true;

    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ----------------------------- 拉取模式 ----------------------------- */

#define MANIFEST_MAX 1024

/** 极简字段提取：在 JSON 文本里找 "key":"value"，够用且不引入依赖。 */
static bool json_field(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (end == NULL) {
            return false;
        }
        size_t n = (size_t)(end - p);
        if (n >= out_len) {
            n = out_len - 1;
        }
        memcpy(out, p, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

void ota_remote_check_manifest(void)
{
#ifdef APP_OTA_BASE_URL
    if (s_busy) {
        return;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/local/switchyard/%s/firmware.json",
             APP_OTA_BASE_URL, APP_DEVICE_ID);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return;
    }

    static char body[MANIFEST_MAX];
    int total = 0;
    body[0] = '\0';
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) == 200) {
            for (;;) {
                const int n = esp_http_client_read(client, body + total, MANIFEST_MAX - 1 - total);
                if (n <= 0) {
                    break;
                }
                total += n;
                if (total >= MANIFEST_MAX - 1) {
                    break;
                }
            }
            body[total] = '\0';
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total == 0) {
        ESP_LOGD(TAG, "manifest 不可用，跳过自动升级");
        return;
    }

    char remote_version[32] = {0};
    char file[64] = {0};
    if (!json_field(body, "version", remote_version, sizeof(remote_version))) {
        return;
    }
    if (!json_field(body, "file", file, sizeof(file))) {
        strlcpy(file, "firmware.bin", sizeof(file));
    }

    const char *current = esp_app_get_description()->version;
    if (strcmp(remote_version, current) == 0) {
        ESP_LOGD(TAG, "已是最新版本 %s", current);
        return;
    }

    ESP_LOGI(TAG, "发现新固件 %s（当前 %s），开始升级", remote_version, current);
    char fw_url[256];
    snprintf(fw_url, sizeof(fw_url), "%s/local/switchyard/%s/%s", APP_OTA_BASE_URL, APP_DEVICE_ID, file);
    ota_remote_start(fw_url, remote_version);
#else
    (void)0;
#endif
}

bool ota_remote_busy(void)
{
    return s_busy;
}
