#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "esp_check.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_state.h"
#include "leak_sensor.h"
#include "mqtt_manager.h"
#include "valve.h"

static const char *TAG = "leak";

/* 由检测任务写、其它任务只读 */
static volatile bool s_wet = false;

bool leak_sensor_is_wet(void)
{
    return s_wet;
}

static void leak_task(void *arg)
{
    (void)arg;

    int wet_samples = 0;
    int dry_samples = 0;
    bool stable_wet = false;

    for (;;) {
        /* 浸水时传感器把引脚拉低；干态靠内部上拉保持高电平 */
        const bool raw_wet = gpio_get_level(PIN_LEAK_SENSOR) == 0;

        if (raw_wet) {
            wet_samples++;
            dry_samples = 0;
        } else {
            dry_samples++;
            wet_samples = 0;
        }

        if (!stable_wet && wet_samples >= LEAK_DEBOUNCE_SAMPLES) {
            stable_wet = true;
            s_wet = true;

            app_state_set_leak(true);
            ESP_LOGE(TAG, "======== 检测到漏水！立即本地关阀 ========");

            /* 本地闭环：不经过 MQTT / HA，直接投递关阀指令 */
            valve_request(VALVE_SOURCE_LEAK, false);

            app_state_set_last_event("leak_detected");
            mqtt_publish_event("leak_detected");
            mqtt_publish_leak(true);

        } else if (stable_wet && dry_samples >= LEAK_DEBOUNCE_SAMPLES) {
            stable_wet = false;
            s_wet = false;

            app_state_set_leak(false);
            ESP_LOGW(TAG, "漏水已解除；阀门保持关闭，需人工确认后手动开启");

            app_state_set_last_event("leak_cleared");
            mqtt_publish_event("leak_cleared");
            mqtt_publish_leak(false);
        }

        vTaskDelay(pdMS_TO_TICKS(LEAK_POLL_MS));
    }
}

esp_err_t leak_sensor_init(void)
{
    const gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << PIN_LEAK_SENSOR,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "配置漏水检测引脚失败");

    /* 上电先读一次真实状态，避免带着"干态"假设启动 */
    const bool wet_now = gpio_get_level(PIN_LEAK_SENSOR) == 0;
    s_wet = wet_now;
    app_state_set_leak(wet_now);
    if (wet_now) {
        ESP_LOGE(TAG, "启动时即检测到漏水，请检查现场");
    }

    ESP_LOGI(TAG, "漏水检测就绪 (GPIO%d, 浸水拉低, 去抖 %d×%d ms)",
             PIN_LEAK_SENSOR, LEAK_DEBOUNCE_SAMPLES, LEAK_POLL_MS);
    return ESP_OK;
}

esp_err_t leak_sensor_start(void)
{
    const BaseType_t created = xTaskCreate(leak_task, "leak", TASK_STACK_LEAK, NULL,
                                           TASK_PRIO_LEAK, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "创建漏水检测任务失败");
    return ESP_OK;
}
