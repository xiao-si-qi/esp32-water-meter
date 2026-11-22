/**
 * @file telemetry.h
 * @brief 周期抄表任务：水量、阀门状态、电池、信号强度。
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include "esp_err.h"

/** @brief 启动遥测任务（启动后立即执行一轮，之后每 TELEMETRY_PERIOD_MS 一轮）。 */
esp_err_t telemetry_start(void);

#endif /* TELEMETRY_H */
