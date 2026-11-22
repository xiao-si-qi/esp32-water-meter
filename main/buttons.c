#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "esp_check.h"
#include "esp_log.h"

#include "app_config.h"
#include "buttons.h"
#include "valve.h"

static const char *TAG = "button";

typedef struct {
    gpio_num_t pin;
    const char *name;
    bool    stable_level;   /* 上拉输入，未按下为高电平 */
    int     counter;
} button_t;

static bool button_pressed_edge(button_t *button)
{
    const bool raw_level = gpio_get_level(button->pin) != 0;

    if (raw_level == button->stable_level) {
        button->counter = 0;
        return false;
    }

    if (++button->counter < BUTTON_DEBOUNCE_SAMPLES) {
        return false;
    }

    button->counter = 0;
    button->stable_level = raw_level;
    return raw_level == false;   /* 下降沿即按下 */
}

static void buttons_task(void *arg)
{
    (void)arg;

    button_t open_button = {.pin = PIN_BUTTON_OPEN, .name = "开阀", .stable_level = true, .counter = 0};
    button_t close_button = {.pin = PIN_BUTTON_CLOSE, .name = "关阀", .stable_level = true, .counter = 0};

    for (;;) {
        if (button_pressed_edge(&open_button)) {
            ESP_LOGI(TAG, "%s按键按下", open_button.name);
            valve_request(VALVE_SOURCE_BUTTON, true);
        }
        if (button_pressed_edge(&close_button)) {
            ESP_LOGI(TAG, "%s按键按下", close_button.name);
            valve_request(VALVE_SOURCE_BUTTON, false);
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t buttons_init(void)
{
    const gpio_config_t button_io_cfg = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_OPEN) | (1ULL << PIN_BUTTON_CLOSE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_io_cfg), TAG, "配置按键引脚失败");

    ESP_LOGI(TAG, "按键就绪 (开阀=GPIO%d, 关阀=GPIO%d)", PIN_BUTTON_OPEN, PIN_BUTTON_CLOSE);
    return ESP_OK;
}

esp_err_t buttons_start(void)
{
    const BaseType_t created = xTaskCreate(buttons_task, "button", TASK_STACK_BUTTONS, NULL,
                                           TASK_PRIO_BUTTONS, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "创建按键任务失败");
    return ESP_OK;
}
