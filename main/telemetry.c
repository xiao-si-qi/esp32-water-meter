#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_state.h"
#include "battery.h"
#include "modbus_rtu.h"
#include "mqtt_manager.h"
#include "telemetry.h"
#include "water_meter.h"
#include "wifi_manager.h"

static const char *TAG = "telemetry";

/* 原实现把"读水量 + 读阀门 + 读电池 + 上报"全部塞在 modbus 任务里，
 * 现在拆成独立任务，且阀门动作期间主动让出总线控制权给阀门任务。 */
static void telemetry_cycle(void)
{
    const bool was_failed = app_state_comm_failed();
    bool failed = false;
    modbus_fault_t primary_fault = MODBUS_FAULT_NONE;

    /* ---------- 累计水量 ---------- */
    float volume_m3 = 0.0f;
    modbus_fault_t fault = MODBUS_FAULT_NONE;
    if (water_meter_read_volume(&volume_m3, &fault) == ESP_OK) {
        app_state_set_volume(volume_m3);
        mqtt_publish_water(volume_m3);
        ESP_LOGI(TAG, "💧 累计水量 %.2f m³", volume_m3);
    } else {
        failed = true;
        primary_fault = fault;
        ESP_LOGW(TAG, "水量读取失败: %s", modbus_fault_name(fault));
    }

    /* ---------- 阀门状态 ---------- */
    if (!app_state_valve_moving()) {
        bool is_open = false;
        fault = MODBUS_FAULT_NONE;
        if (water_meter_read_valve(&is_open, &fault) == ESP_OK) {
            app_state_set_valve(is_open, false, is_open);
            mqtt_publish_valve_state(is_open);
            ESP_LOGI(TAG, "🔧 阀门状态: %s", is_open ? "开" : "关");
        } else {
            failed = true;
            if (primary_fault == MODBUS_FAULT_NONE) {
                primary_fault = fault;   /* 保留第一个失败原因，更有代表性 */
            }
            ESP_LOGW(TAG, "阀门状态读取失败: %s", modbus_fault_name(fault));
        }
    } else {
        ESP_LOGI(TAG, "阀门正在动作，本轮跳过状态读取");
    }

    /* ---------- 电池与信号 ---------- */
    float voltage_v = 0.0f;
    int percent = 0;
    if (battery_read(&voltage_v, &percent) == ESP_OK) {
        app_state_set_battery(voltage_v, percent);
        mqtt_publish_battery(voltage_v, percent);
        ESP_LOGI(TAG, "🔋 %.3f V, 电量 %d%%", voltage_v, percent);
    } else {
        ESP_LOGW(TAG, "电池采样失败");
    }

    int rssi_dbm = 0;
    if (wifi_manager_get_rssi(&rssi_dbm) == ESP_OK) {
        app_state_set_rssi(rssi_dbm);
    }

    /* ---------- 汇总与上报 ---------- */
    app_state_set_comm_fail(failed);

    if (failed && !was_failed) {
        /* 只在"由好变坏"的边沿发事件，避免每 60 秒刷一条同样的故障 */
        const char *event = modbus_fault_event(primary_fault);
        ESP_LOGW(TAG, "通信故障开始: %s", modbus_fault_name(primary_fault));
        app_state_set_last_event(event);
        mqtt_publish_event(event);
    } else if (!failed && was_failed) {
        ESP_LOGI(TAG, "通信已恢复");
        app_state_set_last_event("comm_ok");
        mqtt_publish_event("comm_ok");
    }

    mqtt_publish_comm_fault(failed);

    app_state_t state;
    app_state_snapshot(&state);
    mqtt_publish_diagnostics(state.ip_addr, state.rssi_dbm);
}

static void telemetry_task(void *arg)
{
    (void)arg;

    for (;;) {
        telemetry_cycle();
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}

esp_err_t telemetry_start(void)
{
    const BaseType_t created = xTaskCreate(telemetry_task, "telemetry", TASK_STACK_TELEMETRY,
                                           NULL, TASK_PRIO_TELEMETRY, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "创建遥测任务失败");
    return ESP_OK;
}
