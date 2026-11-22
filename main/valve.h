/**
 * @file valve.h
 * @brief 阀门控制任务。
 *
 * 同时支持三个操作来源，全部汇入同一个指令队列：
 *   - 本地按键（无需网络）
 *   - 本地网页（局域网 HTTP）
 *   - Home Assistant / MQTT（远程）
 *
 * 冲突策略：**最新指令生效**（队列长度 1 + 覆盖写入）。
 *   - 任何来源的指令都不会被丢弃；
 *   - 机械动作期间到达的新指令会排队，当前动作完成后立即执行；
 *   - 指令执行前会先读水表真实状态，已处于目标状态则跳过，避免无谓动作。
 *
 * 原实现的 valve_control_execute() 是同步函数，被 MQTT 事件回调直接调用，
 * 内部要阻塞 12 秒等机械动作、再最多 5x2 秒轮询确认，累计可占用 MQTT 事件
 * 任务 22 秒以上。重构后调用方只投递指令（毫秒级返回）。
 */
#ifndef VALVE_H
#define VALVE_H

#include <stdbool.h>

#include "esp_err.h"

/** @brief 指令来源，仅用于日志与排查，不改变优先级 */
typedef enum {
    VALVE_SOURCE_BUTTON = 0,
    VALVE_SOURCE_WEB,
    VALVE_SOURCE_MQTT,
    VALVE_SOURCE_LEAK,       /* 漏水联动：最高优先级的关阀来源 */
} valve_source_t;

/** @brief 阀门状态变化回调：is_open=当前状态，is_moving=是否正在动作。 */
typedef void (*valve_state_listener_t)(bool is_open, bool is_moving);

/**
 * @brief 阀门异常事件回调，用于上报给 HA 触发通知。
 *
 * @param event        事件名（valve_ok / valve_cmd_failed / valve_not_confirmed）
 * @param fault_active true 表示当前存在故障，false 表示已恢复正常
 */
typedef void (*valve_event_listener_t)(const char *event, bool fault_active);

/** @brief 初始化引脚、指令队列并启动阀门任务。 */
esp_err_t valve_init(valve_state_listener_t state_listener,
                     valve_event_listener_t event_listener);

/**
 * @brief 投递阀门开/关请求（非阻塞）。
 *
 * 队列满时覆盖旧指令而不是失败，因此三个来源并发调用都不会丢指令。
 */
esp_err_t valve_request(valve_source_t source, bool open);

#endif /* VALVE_H */
