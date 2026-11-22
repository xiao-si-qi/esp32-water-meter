/**
 * @file main.c
 * @brief 启动装配：只做初始化编排，不含任何业务逻辑。
 *
 * 初始化顺序有依赖，不能随意调整：
 *   app_state  —— 其它模块都依赖它
 *   leds/buttons/battery —— 纯外设，无依赖
 *   water_meter -> valve —— water_meter 内部会先初始化 RS485 总线，阀门任务依赖它
 *   wifi -> telemetry -> aht10 —— 遥测上报依赖网络模块已初始化（未连接时会静默丢弃）
 */
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "aht10_sensor.h"
#include "app_state.h"
#include "battery.h"
#include "buttons.h"
#include "leak_sensor.h"
#include "leds.h"
#include "mqtt_manager.h"
#include "telemetry.h"
#include "valve.h"
#include "water_meter.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "========== ESP32-C3 水表启动 ==========");

    /* 便于确认 OTA 后实际从哪个槽启动 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        ESP_LOGI(TAG, "运行分区: %s @ 0x%05lx", running->label,
                 (unsigned long)running->address);
    }

    ESP_ERROR_CHECK(app_state_init());
    /* 启动事件：让网页与 HA 的「最近事件」在重启后立刻有明确内容，
     * 同时覆盖掉上一次会话残留的故障事件 */
    app_state_set_last_event("device_boot");

    ESP_ERROR_CHECK(leds_init());
    ESP_ERROR_CHECK(buttons_init());
    ESP_ERROR_CHECK(battery_init());

    ESP_ERROR_CHECK(water_meter_init());
    /* 阀门状态变化直接回调到 MQTT 上报，两个模块之间不产生编译期依赖 */
    ESP_ERROR_CHECK(valve_init(mqtt_manager_on_valve_state, mqtt_manager_on_valve_event));

    ESP_ERROR_CHECK(leds_start());
    ESP_ERROR_CHECK(buttons_start());

    /*
     * 漏水检测放在阀门初始化之后：漏水联动会投递关阀指令，
     * 必须先保证阀门模块（队列 + 任务）已就绪。
     */
    ESP_ERROR_CHECK(leak_sensor_init());
    ESP_ERROR_CHECK(leak_sensor_start());

    /* 只创建连接监督任务；真正的建连由 Wi-Fi 拿到 IP 后触发 */
    ESP_ERROR_CHECK(mqtt_manager_init());

    ESP_ERROR_CHECK(wifi_manager_start());

    const esp_err_t web_err = web_server_start();
    if (web_err != ESP_OK) {
        /* 网页不可用不应影响抄表与 MQTT 上报 */
        ESP_LOGW(TAG, "网页服务启动失败 (%s)", esp_err_to_name(web_err));
    }

    ESP_ERROR_CHECK(telemetry_start());

    const esp_err_t climate_err = aht10_start();
    if (climate_err != ESP_OK) {
        /* 传感器缺失不应导致整机无法工作，降级处理 */
        ESP_LOGW(TAG, "AHT10 启动失败 (%s)，温湿度上报不可用", esp_err_to_name(climate_err));
    }

    ESP_LOGI(TAG, "初始化完成");
}
