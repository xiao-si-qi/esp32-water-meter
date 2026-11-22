#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c.h"

#include "aht.h"
#include "esp_check.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_state.h"
#include "aht10_sensor.h"
#include "mqtt_manager.h"

static const char *TAG = "aht10";

#define AHT10_I2C_PORT   I2C_NUM_0
#define AHT10_PIN_SDA    GPIO_NUM_4
#define AHT10_PIN_SCL    GPIO_NUM_5

static aht_t s_aht_dev;
static bool s_started = false;

static void aht10_task(void *arg)
{
    (void)arg;

    for (;;) {
        float temperature_c = 0.0f;
        float humidity_pct = 0.0f;

        const esp_err_t err = aht_get_data(&s_aht_dev, &temperature_c, &humidity_pct);
        if (err == ESP_OK) {
            app_state_set_climate(temperature_c, humidity_pct);
            ESP_LOGI(TAG, "🌡️ %.2f °C, 💧 %.2f %%", temperature_c, humidity_pct);
            mqtt_publish_climate(temperature_c, humidity_pct);
        } else {
            ESP_LOGW(TAG, "读取失败: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(CLIMATE_PERIOD_MS));
    }
}

esp_err_t aht10_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2cdev_init(), TAG, "初始化 I2C 总线失败");

    esp_err_t err = aht_init_desc(&s_aht_dev, AHT_I2C_ADDRESS_GND,
                                  AHT10_I2C_PORT, AHT10_PIN_SDA, AHT10_PIN_SCL);
    ESP_RETURN_ON_ERROR(err, TAG, "创建 AHT10 描述符失败");

    err = aht_init(&s_aht_dev);
    ESP_RETURN_ON_ERROR(err, TAG, "AHT10 初始化失败");

    const BaseType_t created = xTaskCreate(aht10_task, "aht10", TASK_STACK_CLIMATE, NULL,
                                           TASK_PRIO_CLIMATE, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "创建温湿度任务失败");

    s_started = true;
    ESP_LOGI(TAG, "AHT10 就绪 (I2C0 SDA=%d SCL=%d)", AHT10_PIN_SDA, AHT10_PIN_SCL);
    return ESP_OK;
}
