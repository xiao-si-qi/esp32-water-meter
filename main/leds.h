/**
 * @file leds.h
 * @brief 所有指示灯的唯一所有者。
 *
 * 原实现里状态灯、阀门灯、MQTT 活动灯分散在 peripherals.c 和 mqtt_manager.c，
 * MQTT 模块直接操作 GPIO，职责越界。现在 GPIO 只由本模块操作，
 * MQTT 侧通过 leds_notify_mqtt_activity() 上报“有活动”这一事件即可。
 */
#ifndef LEDS_H
#define LEDS_H

#include "esp_err.h"

/** @brief 配置三个指示灯引脚。 */
esp_err_t leds_init(void);

/** @brief 启动 LED 刷新任务。 */
esp_err_t leds_start(void);

/** @brief 上报一次 MQTT 收发活动（点亮活动灯 LED_MQTT_ACTIVITY_MS）。 */
void leds_notify_mqtt_activity(void);

#endif /* LEDS_H */
