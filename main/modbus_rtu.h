/**
 * @file modbus_rtu.h
 * @brief Modbus RTU 主站协议层（与具体设备无关）。
 *
 * 分层职责：
 *   rs485      —— 只管字节进字节出
 *   modbus_rtu —— 组帧、CRC 校验、异常码识别（本文件）
 *   water_meter—— 寄存器地址映射与量纲转换
 *
 * 所有失败都会带上 modbus_fault_t 分类，而不是笼统的"通信失败"。
 * 不同分类对应完全不同的排查方向，这是把它单独建模的原因。
 */
#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 功能码 */
#define MODBUS_FUNC_READ_COILS        0x01
#define MODBUS_FUNC_READ_HOLDING      0x03
#define MODBUS_FUNC_WRITE_SINGLE_COIL 0x05

#define MODBUS_MAX_READ_REGISTERS     16
#define MODBUS_MAX_PDU_LEN            32
#define MODBUS_MAX_FRAME_LEN          64
#define MODBUS_MIN_FRAME_LEN           5   /* 从站地址 + 功能码 + 数据 + CRC(2) */
#define MODBUS_TIMEOUT_MS            300

/** @brief 通信失败分类，每一类指向不同的排查方向 */
typedef enum {
    MODBUS_FAULT_NONE = 0,
    MODBUS_FAULT_TIMEOUT,       /* 从站无应答：接线 / 供电 / 从站地址 / 波特率 */
    MODBUS_FAULT_TX,            /* 本机串口发送失败：驱动或引脚问题 */
    MODBUS_FAULT_CRC,           /* CRC 不符：干扰、终端电阻、地线 */
    MODBUS_FAULT_EXCEPTION,     /* 从站返回异常码：从站明确拒绝执行 */
    MODBUS_FAULT_BAD_FRAME,     /* 从站地址或功能码不符：多机地址冲突 */
    MODBUS_FAULT_BAD_LENGTH,    /* 应答长度与预期不符：帧被截断或串扰 */
} modbus_fault_t;

/** @brief 中文描述，用于日志 */
const char *modbus_fault_name(modbus_fault_t fault);

/** @brief 事件名（如 comm_crc），用于 MQTT 上报与 HA 自动化 */
const char *modbus_fault_event(modbus_fault_t fault);

/** @brief Modbus CRC16（多项式 0xA001，初值 0xFFFF，低字节在前）。 */
uint16_t modbus_crc16(const uint8_t *data, size_t len);

/**
 * @brief 发送一个 PDU 并取回应答 PDU。
 *
 * @param slave       从站地址
 * @param pdu         请求 PDU（首字节为功能码，不含从站地址与 CRC）
 * @param rx_pdu      输出：应答 PDU（去除从站地址与 CRC）
 * @param rx_capacity rx_pdu 容量
 * @param rx_pdu_len  输出：应答 PDU 长度
 * @param fault       输出：失败分类，可为 NULL
 */
esp_err_t modbus_transfer(uint8_t slave, const uint8_t *pdu, size_t pdu_len,
                          uint8_t *rx_pdu, size_t rx_capacity, size_t *rx_pdu_len,
                          uint32_t timeout_ms, modbus_fault_t *fault);

/** @brief 0x03 读保持寄存器。 */
esp_err_t modbus_read_holding_registers(uint8_t slave, uint16_t address, uint16_t count,
                                        uint16_t *values, modbus_fault_t *fault);

/** @brief 0x05 写单个线圈。 */
esp_err_t modbus_write_single_coil(uint8_t slave, uint16_t coil, bool on,
                                   modbus_fault_t *fault);

#endif /* MODBUS_RTU_H */
