/**
 * @file aht10_sensor.h
 * @brief AHT10 温湿度传感器（I2C0）。
 */
#ifndef AHT10_SENSOR_H
#define AHT10_SENSOR_H

#include "esp_err.h"

/** @brief 初始化 I2C 总线与传感器，成功后启动周期采样任务。 */
esp_err_t aht10_start(void);

#endif /* AHT10_SENSOR_H */
