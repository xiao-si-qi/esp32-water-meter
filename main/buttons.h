/**
 * @file buttons.h
 * @brief 开阀 / 关阀两个实体按键的消抖处理。
 *
 * 消抖逻辑从 peripherals.c 抽出来，按键只负责“产生阀门请求”，
 * 不直接碰 GPIO10，也不判断阀门当前状态（那是 valve 模块的职责）。
 */
#ifndef BUTTONS_H
#define BUTTONS_H

#include "esp_err.h"

/** @brief 配置按键输入引脚（内部上拉）。 */
esp_err_t buttons_init(void);

/** @brief 启动按键扫描任务。 */
esp_err_t buttons_start(void);

#endif /* BUTTONS_H */
