#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_check.h"

#include "app_state.h"

static const char *TAG = "state";

static app_state_t s_state;
static SemaphoreHandle_t s_state_lock = NULL;

static inline void state_lock(void)
{
    if (s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}

static inline void state_unlock(void)
{
    if (s_state_lock != NULL) {
        xSemaphoreGive(s_state_lock);
    }
}

esp_err_t app_state_init(void)
{
    if (s_state_lock != NULL) {
        return ESP_OK;
    }

    s_state_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_state_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建状态互斥锁失败");

    memset(&s_state, 0, sizeof(s_state));
    snprintf(s_state.ip_addr, sizeof(s_state.ip_addr), "0.0.0.0");
    return ESP_OK;
}

void app_state_snapshot(app_state_t *out)
{
    if (out == NULL) {
        return;
    }
    state_lock();
    *out = s_state;
    state_unlock();
}

void app_state_set_wifi(bool connected)
{
    state_lock();
    s_state.wifi_connected = connected;
    if (!connected) {
        snprintf(s_state.ip_addr, sizeof(s_state.ip_addr), "0.0.0.0");
    }
    state_unlock();
}

void app_state_set_ip(const char *ip_addr)
{
    state_lock();
    if (ip_addr != NULL) {
        snprintf(s_state.ip_addr, sizeof(s_state.ip_addr), "%s", ip_addr);
    }
    state_unlock();
}

void app_state_set_mqtt(bool connected)
{
    state_lock();
    s_state.mqtt_connected = connected;
    state_unlock();
}

void app_state_set_rssi(int rssi_dbm)
{
    state_lock();
    s_state.rssi_dbm = rssi_dbm;
    state_unlock();
}

void app_state_set_comm_fail(bool failed)
{
    state_lock();
    s_state.comm_fail = failed;
    state_unlock();
}

void app_state_set_valve_fault(bool faulty)
{
    state_lock();
    s_state.valve_fault = faulty;
    state_unlock();
}

void app_state_set_leak(bool detected)
{
    state_lock();
    s_state.leak_detected = detected;
    state_unlock();
}

void app_state_set_last_event(const char *event)
{
    if (event == NULL) {
        return;
    }
    state_lock();
    snprintf(s_state.last_event, sizeof(s_state.last_event), "%s", event);
    state_unlock();
}

void app_state_set_volume(float volume_m3)
{
    state_lock();
    s_state.volume_m3 = volume_m3;
    state_unlock();
}

void app_state_set_climate(float temperature_c, float humidity_pct)
{
    state_lock();
    s_state.temperature_c = temperature_c;
    s_state.humidity_pct = humidity_pct;
    state_unlock();
}

void app_state_set_battery(float voltage_v, int percent)
{
    state_lock();
    s_state.battery_voltage_v = voltage_v;
    s_state.battery_percent = percent;
    state_unlock();
}

void app_state_set_valve(bool is_open, bool is_moving, bool target_open)
{
    state_lock();
    s_state.valve_open = is_open;
    s_state.valve_moving = is_moving;
    s_state.valve_target_open = target_open;
    state_unlock();
}

bool app_state_wifi_connected(void)
{
    state_lock();
    const bool value = s_state.wifi_connected;
    state_unlock();
    return value;
}

bool app_state_mqtt_connected(void)
{
    state_lock();
    const bool value = s_state.mqtt_connected;
    state_unlock();
    return value;
}

bool app_state_comm_failed(void)
{
    state_lock();
    const bool value = s_state.comm_fail;
    state_unlock();
    return value;
}

bool app_state_leak_detected(void)
{
    state_lock();
    const bool value = s_state.leak_detected;
    state_unlock();
    return value;
}

bool app_state_valve_open(void)
{
    state_lock();
    const bool value = s_state.valve_open;
    state_unlock();
    return value;
}

bool app_state_valve_moving(void)
{
    state_lock();
    const bool value = s_state.valve_moving;
    state_unlock();
    return value;
}

bool app_state_valve_target(void)
{
    state_lock();
    const bool value = s_state.valve_target_open;
    state_unlock();
    return value;
}
