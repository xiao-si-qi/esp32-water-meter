/**
 * @file log_buffer.h
 * @brief 最近日志的环形缓冲 + 错误摘要（远程诊断用）。
 *
 * 设计取舍：**不把日志流推 MQTT**（噪声大、会撑爆 HA 历史、可能带敏感信息）。
 * 只做两件事：
 *   1. 保留最近 N 行在 RAM 里，设备被要求时才发一次（非 retained，看完即弃）；
 *   2. 出现 W/E 级日志时记一条摘要，由上报任务限流发到 /diag（retained）。
 */
#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

/** @brief 安装日志钩子（在 mqtt_manager_init 里调一次即可）。 */
void log_buffer_init(void);

/**
 * @brief 取最近若干行日志，用 '\n' 连接。
 *
 * @param out    输出缓冲
 * @param len    缓冲长度
 * @param lines  要取的行数（从新到旧取，按时间正序写入）
 * @return 实际写入的字节数
 */
size_t log_buffer_dump(char *out, size_t len, int lines);

/**
 * @brief 取"自上次取走以来"的第一条 W/E 日志摘要。
 *
 * @param out  输出缓冲
 * @param len  缓冲长度
 * @return true 表示有新错误（已写入 out，格式为 JSON）
 */
bool log_buffer_take_diag(char *out, size_t len);

#endif /* LOG_BUFFER_H */
