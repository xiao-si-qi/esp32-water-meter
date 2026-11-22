#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "esp_check.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_state.h"
#include "leds.h"

static const char *TAG = "leds";

/* 由 MQTT 模块写入、LED 任务读取，TickType_t 在 32 位平台上原子读写 */
static volatile TickType_t s_last_activity_tick = 0;

void leds_notify_mqtt_activity(void)
{
    s_last_activity_tick = xTaskGetTickCount();
}

/**
 * @brief 系统状态灯：联网正常慢闪，异常快闪。
 *
 * 原实现用 vTaskDelay 拼接明暗时间，改配置就得重算两处延时；
 * 这里改成“相位累加 + 与阈值比较”，一个循环统一所有节奏。
 */
static void update_status_led(const app_state_t *state, uint32_t *phase_ms)
{
    const bool healthy = state->wifi_connected && state->mqtt_connected;
    const uint32_t on_ms = healthy ? 500U : 100U;
    const uint32_t off_ms = healthy ? 1500U : 100U;
    const uint32_t period_ms = on_ms + off_ms;

    *phase_ms = (*phase_ms + LED_TICK_MS) % period_ms;
    gpio_set_level(PIN_STATUS_LED, (*phase_ms < on_ms) ? 1 : 0);
}

/**
 * @brief 阀门指示灯：通信故障双灯齐闪，动作中闪目标侧，静止时常亮当前侧。
 */
static void update_valve_leds(const app_state_t *state, uint32_t *phase_ms)
{
    if (state->comm_fail) {
        *phase_ms = (*phase_ms + LED_TICK_MS) % 500U;
        const int level = (*phase_ms < 250U) ? 1 : 0;
        gpio_set_level(PIN_LED_VALVE_OPEN, level);
        gpio_set_level(PIN_LED_VALVE_CLOSE, level);
        return;
    }

    if (state->valve_moving) {
        *phase_ms = (*phase_ms + LED_TICK_MS) % 200U;
        const bool lit = *phase_ms < 100U;
        gpio_set_level(PIN_LED_VALVE_OPEN, (lit && state->valve_target_open) ? 1 : 0);
        gpio_set_level(PIN_LED_VALVE_CLOSE, (lit && !state->valve_target_open) ? 1 : 0);
        return;
    }

    gpio_set_level(PIN_LED_VALVE_OPEN, state->valve_open ? 1 : 0);
    gpio_set_level(PIN_LED_VALVE_CLOSE, state->valve_open ? 0 : 1);
}

static void update_mqtt_led(void)
{
    const TickType_t now = xTaskGetTickCount();
    const bool active = (now - s_last_activity_tick) < pdMS_TO_TICKS(LED_MQTT_ACTIVITY_MS);
    gpio_set_level(PIN_MQTT_LED, active ? 1 : 0);
}

static void leds_task(void *arg)
{
    (void)arg;

    uint32_t status_phase_ms = 0;
    uint32_t valve_phase_ms = 0;

    for (;;) {
        app_state_t state;
        app_state_snapshot(&state);

        update_status_led(&state, &status_phase_ms);
        update_valve_leds(&state, &valve_phase_ms);
        update_mqtt_led();

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

esp_err_t leds_init(void)
{
    const gpio_config_t led_io_cfg = {
        .pin_bit_mask = (1ULL << PIN_STATUS_LED) | (1ULL << PIN_MQTT_LED) |
                        (1ULL << PIN_LED_VALVE_OPEN) | (1ULL << PIN_LED_VALVE_CLOSE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&led_io_cfg), TAG, "配置指示灯引脚失败");

    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_STATUS_LED, 0), TAG, "初始化状态灯失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_MQTT_LED, 0), TAG, "初始化 MQTT 灯失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_LED_VALVE_OPEN, 0), TAG, "初始化开阀灯失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_LED_VALVE_CLOSE, 0), TAG, "初始化关阀灯失败");
    return ESP_OK;
}

esp_err_t leds_start(void)
{
    const BaseType_t created = xTaskCreate(leds_task, "leds", TASK_STACK_LEDS, NULL,
                                           TASK_PRIO_LEDS, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "创建 LED 任务失败");
    return ESP_OK;
}
