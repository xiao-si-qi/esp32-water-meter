/**
 * @file mqtt_manager.h
 * @brief MQTT 连接、Home Assistant 自动发现与状态上报。
 *
 * 同时支持明文 mqtt:// 与加密 mqtts://，传输方式由配置中的 URI 决定：
 *   - mqtt://   直接连接
 *   - mqtts://  先等 SNTP 对时完成（否则证书校验必失败），再建立 TLS 连接
 *
 * 对外只暴露“上报某个量”的语义化函数，调用方无需关心 Topic 拼接与连接状态；
 * 未连接时所有上报静默丢弃，不会阻塞调用者。
 */
#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

/** @brief 在 main 中调用一次：创建连接监督任务（不发起连接）。 */
esp_err_t mqtt_manager_init(void);

/**
 * @brief 请求建立连接（幂等）。
 *
 * 由 Wi-Fi 拿到 IP 后调用；真正建连在监督任务中完成，不会阻塞调用者。
 */
esp_err_t mqtt_manager_start(void);

/** @brief 重新载入配置并重连（改完 mqtt_settings 后调用）。 */
esp_err_t mqtt_manager_restart(void);

/** @brief 阀门状态变化回调，由 valve 模块通过 valve_init() 注册调用。 */
void mqtt_manager_on_valve_state(bool is_open, bool is_moving);

/**
 * @brief 阀门异常事件回调，由 valve 模块通过 valve_init() 注册调用。
 *
 * 发布到 `ha/watermeter/event` 供 HA 自动化触发通知，同时更新
 * `Valve Fault` 二进制传感器。
 */
void mqtt_manager_on_valve_event(const char *event, bool fault_active);

/** @brief 上报一条事件文本（valve_ok / valve_cmd_failed / valve_not_confirmed）。 */
void mqtt_publish_event(const char *event);

/** @brief 更新阀门故障二进制传感器。 */
void mqtt_publish_valve_fault(bool fault);

/** @brief 更新漏水二进制传感器（device_class: moisture）。 */
void mqtt_publish_leak(bool detected);

/** @brief 更新 RS485 通信故障二进制传感器。 */
void mqtt_publish_comm_fault(bool fault);

void mqtt_publish_water(float volume_m3);
void mqtt_publish_valve_state(bool is_open);
void mqtt_publish_battery(float voltage_v, int percent);
void mqtt_publish_climate(float temperature_c, float humidity_pct);
void mqtt_publish_diagnostics(const char *ip_addr, int rssi_dbm);

/** @brief 一次性上报全部当前状态（连接建立或发现配置后调用）。 */
void mqtt_publish_all(void);

#endif /* MQTT_MANAGER_H */
