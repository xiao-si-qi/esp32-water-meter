/**
 * @file water_meter.h
 * @brief 水表业务语义层：把 Modbus 寄存器翻译成"水量 / 阀门状态"。
 *
 * 只有本文件知道寄存器地址和量纲，上层（阀门控制、遥测）不接触任何 Modbus 细节。
 * 所有接口都带 fault 出参，失败时能区分"无应答 / CRC 错 / 从站拒绝"等原因。
 */
#ifndef WATER_METER_H
#define WATER_METER_H

#include <stdbool.h>

#include "esp_err.h"
#include "modbus_rtu.h"

/** @brief 初始化 RS485 总线并打印设备参数（不做总线探测，避免阻塞启动）。 */
esp_err_t water_meter_init(void);

/** @brief 读取累计水量，单位 m³。 */
esp_err_t water_meter_read_volume(float *volume_m3, modbus_fault_t *fault);

/** @brief 读取阀门当前状态。 */
esp_err_t water_meter_read_valve(bool *is_open, modbus_fault_t *fault);

/** @brief 下发阀门开/关指令（仅下指令，机械动作需由调用方等待）。 */
esp_err_t water_meter_write_valve(bool open, modbus_fault_t *fault);

#endif /* WATER_METER_H */
