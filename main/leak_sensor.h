/**
 * @file leak_sensor.h
 * @brief 漏水检测与本地联动关阀。
 *
 * 设计要点：**完全本地闭环**，不依赖 WiFi / MQTT / Home Assistant。
 * 检测到漏水就直接调用 valve_request() 关阀，即使断网也能保护现场。
 *
 * 接线：传感器 DO → PIN_LEAK_SENSOR，GND → GND。
 * 引脚内部上拉，干态为高电平，浸水时被拉低到 GND。
 *
 * 故障安全策略：
 *   - 触发后**保持阀门关闭**，不自动开阀（需要人工确认漏水已排除后手动开）；
 *   - 漏水未解除期间，网页 / HA / 按键的"开阀"指令一律拒绝。
 */
#ifndef LEAK_SENSOR_H
#define LEAK_SENSOR_H

#include <stdbool.h>

#include "esp_err.h"

/** @brief 配置检测引脚（内部上拉）。 */
esp_err_t leak_sensor_init(void);

/** @brief 启动漏水检测任务。 */
esp_err_t leak_sensor_start(void);

/** @brief 当前是否检测到漏水。 */
bool leak_sensor_is_wet(void);

#endif /* LEAK_SENSOR_H */
