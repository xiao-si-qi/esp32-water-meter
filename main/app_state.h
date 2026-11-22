/**
 * @file app_state.h
 * @brief 全局运行状态中枢。
 *
 * 原实现用一堆裸 extern 全局变量跨任务共享（g_valve_state / g_mqtt_connected ...），
 * 既无 volatile 也无互斥，多任务读写存在数据竞争。
 * 现在所有共享状态收敛到一个受互斥锁保护的 app_state_t：
 *   - 读：app_state_snapshot() 一次性取一致快照，避免读到撕裂的中间态；
 *   - 写：各 app_state_set_xxx() 内部加锁，调用者无需关心并发。
 * 任何模块不得再自行定义跨任务共享的全局变量。
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    /* 网络 */
    bool  wifi_connected;
    bool  mqtt_connected;
    char  ip_addr[16];
    int   rssi_dbm;

    /* 计量 */
    float volume_m3;
    bool  comm_fail;            /* Modbus 通信故障，用于指示灯告警 */

    /* 故障与事件（网页与 MQTT 共用同一份，避免两处各存一份） */
    bool  valve_fault;
    bool  leak_detected;
    char  last_event[32];

    /* 阀门 */
    bool  valve_open;
    bool  valve_moving;
    bool  valve_target_open;

    /* 环境与电源 */
    float temperature_c;
    float humidity_pct;
    float battery_voltage_v;
    int   battery_percent;
} app_state_t;

/** @brief 创建状态互斥锁，必须在任何其它模块使用状态前调用一次。 */
esp_err_t app_state_init(void);

/** @brief 取状态快照（线程安全）。 */
void app_state_snapshot(app_state_t *out);

void app_state_set_wifi(bool connected);
void app_state_set_ip(const char *ip_addr);
void app_state_set_mqtt(bool connected);
void app_state_set_rssi(int rssi_dbm);
void app_state_set_comm_fail(bool failed);
void app_state_set_valve_fault(bool faulty);
void app_state_set_leak(bool detected);
void app_state_set_last_event(const char *event);
void app_state_set_volume(float volume_m3);
void app_state_set_climate(float temperature_c, float humidity_pct);
void app_state_set_battery(float voltage_v, int percent);
void app_state_set_valve(bool is_open, bool is_moving, bool target_open);

/* 高频读取场景的便捷访问器，内部同样加锁 */
bool app_state_wifi_connected(void);
bool app_state_mqtt_connected(void);
bool app_state_comm_failed(void);
bool app_state_leak_detected(void);
bool app_state_valve_open(void);
bool app_state_valve_moving(void);
bool app_state_valve_target(void);

#endif /* APP_STATE_H */
