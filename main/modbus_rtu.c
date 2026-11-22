#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "modbus_rtu.h"
#include "rs485.h"

static const char *TAG = "modbus";

const char *modbus_fault_name(modbus_fault_t fault)
{
    switch (fault) {
    case MODBUS_FAULT_TIMEOUT:
        return "从站无应答（检查接线 / 水表供电 / 从站地址 / 波特率）";
    case MODBUS_FAULT_TX:
        return "串口发送失败（检查 UART 驱动与引脚）";
    case MODBUS_FAULT_CRC:
        return "CRC 校验失败（检查干扰、终端电阻、地线）";
    case MODBUS_FAULT_EXCEPTION:
        return "从站异常应答（水表拒绝执行该指令）";
    case MODBUS_FAULT_BAD_FRAME:
        return "应答帧异常（从站地址或功能码不符）";
    case MODBUS_FAULT_BAD_LENGTH:
        return "应答长度异常（帧被截断或存在串扰）";
    default:
        return "未知通信故障";
    }
}

const char *modbus_fault_event(modbus_fault_t fault)
{
    switch (fault) {
    case MODBUS_FAULT_TIMEOUT:
        return "comm_timeout";
    case MODBUS_FAULT_TX:
        return "comm_tx";
    case MODBUS_FAULT_CRC:
        return "comm_crc";
    case MODBUS_FAULT_EXCEPTION:
        return "comm_exception";
    case MODBUS_FAULT_BAD_FRAME:
        return "comm_bad_frame";
    case MODBUS_FAULT_BAD_LENGTH:
        return "comm_bad_length";
    default:
        return "comm_unknown";
    }
}

uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void set_fault(modbus_fault_t *fault, modbus_fault_t value)
{
    if (fault != NULL) {
        *fault = value;
    }
}

esp_err_t modbus_transfer(uint8_t slave, const uint8_t *pdu, size_t pdu_len,
                          uint8_t *rx_pdu, size_t rx_capacity, size_t *rx_pdu_len,
                          uint32_t timeout_ms, modbus_fault_t *fault)
{
    set_fault(fault, MODBUS_FAULT_NONE);

    ESP_RETURN_ON_FALSE(pdu != NULL && rx_pdu != NULL && rx_pdu_len != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "参数为空");
    ESP_RETURN_ON_FALSE(pdu_len >= 1 && pdu_len <= MODBUS_MAX_PDU_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "PDU 长度非法: %u", (unsigned)pdu_len);

    uint8_t frame[MODBUS_MAX_PDU_LEN + 3];
    frame[0] = slave;
    memcpy(&frame[1], pdu, pdu_len);

    const uint16_t tx_crc = modbus_crc16(frame, pdu_len + 1);
    frame[pdu_len + 1] = (uint8_t)(tx_crc & 0xFF);
    frame[pdu_len + 2] = (uint8_t)(tx_crc >> 8);

    uint8_t rx[MODBUS_MAX_FRAME_LEN];
    size_t rx_len = 0;
    esp_err_t err = rs485_exchange(frame, pdu_len + 3, rx, sizeof(rx), &rx_len, timeout_ms);
    if (err != ESP_OK) {
        /* 发送失败与"发出去但没人应答"要分开，前者是驱动问题 */
        set_fault(fault, (err == ESP_ERR_TIMEOUT) ? MODBUS_FAULT_TIMEOUT : MODBUS_FAULT_TX);
        ESP_LOGW(TAG, "从站 %u 无有效应答: %s", slave, modbus_fault_name(fault ? *fault : MODBUS_FAULT_TIMEOUT));
        return err;
    }

    if (rx_len < MODBUS_MIN_FRAME_LEN) {
        set_fault(fault, MODBUS_FAULT_BAD_LENGTH);
        ESP_LOGW(TAG, "%s: 收到 %u 字节", modbus_fault_name(MODBUS_FAULT_BAD_LENGTH),
                 (unsigned)rx_len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (rx[0] != slave) {
        set_fault(fault, MODBUS_FAULT_BAD_FRAME);
        ESP_LOGW(TAG, "%s: 期望 %u, 实际 %u",
                 modbus_fault_name(MODBUS_FAULT_BAD_FRAME), slave, rx[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint16_t recv_crc = (uint16_t)rx[rx_len - 2] | ((uint16_t)rx[rx_len - 1] << 8);
    const uint16_t calc_crc = modbus_crc16(rx, rx_len - 2);
    if (recv_crc != calc_crc) {
        set_fault(fault, MODBUS_FAULT_CRC);
        ESP_LOGW(TAG, "%s: 收到 0x%04X, 计算 0x%04X",
                 modbus_fault_name(MODBUS_FAULT_CRC), recv_crc, calc_crc);
        return ESP_ERR_INVALID_CRC;
    }

    const uint8_t function = rx[1];
    if (function & 0x80) {
        set_fault(fault, MODBUS_FAULT_EXCEPTION);
        /* 异常码含义见 Modbus 规范：01 非法功能码 / 02 非法地址 / 03 非法数据 / 04 从站故障 */
        ESP_LOGW(TAG, "%s: 功能码 0x%02X, 异常码 %u",
                 modbus_fault_name(MODBUS_FAULT_EXCEPTION),
                 function & 0x7F, (rx_len > 2) ? rx[2] : 0);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (function != pdu[0]) {
        set_fault(fault, MODBUS_FAULT_BAD_FRAME);
        ESP_LOGW(TAG, "%s: 期望 0x%02X, 实际 0x%02X",
                 modbus_fault_name(MODBUS_FAULT_BAD_FRAME), pdu[0], function);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t payload_len = rx_len - 3;   /* 去掉从站地址与 2 字节 CRC */
    if (payload_len > rx_capacity) {
        set_fault(fault, MODBUS_FAULT_BAD_LENGTH);
        ESP_LOGW(TAG, "%s: %u > %u", modbus_fault_name(MODBUS_FAULT_BAD_LENGTH),
                 (unsigned)payload_len, (unsigned)rx_capacity);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(rx_pdu, &rx[1], payload_len);
    *rx_pdu_len = payload_len;
    return ESP_OK;
}

esp_err_t modbus_read_holding_registers(uint8_t slave, uint16_t address, uint16_t count,
                                        uint16_t *values, modbus_fault_t *fault)
{
    set_fault(fault, MODBUS_FAULT_NONE);

    ESP_RETURN_ON_FALSE(values != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");
    ESP_RETURN_ON_FALSE(count >= 1 && count <= MODBUS_MAX_READ_REGISTERS,
                        ESP_ERR_INVALID_ARG, TAG, "寄存器数量非法: %u", count);

    const uint8_t pdu[5] = {
        MODBUS_FUNC_READ_HOLDING,
        (uint8_t)(address >> 8), (uint8_t)(address & 0xFF),
        (uint8_t)(count >> 8), (uint8_t)(count & 0xFF),
    };

    uint8_t rx[MODBUS_MAX_PDU_LEN];
    size_t rx_len = 0;
    const esp_err_t err = modbus_transfer(slave, pdu, sizeof(pdu), rx, sizeof(rx),
                                          &rx_len, MODBUS_TIMEOUT_MS, fault);
    if (err != ESP_OK) {
        return err;
    }

    const size_t expected_data = (size_t)count * 2U;
    const uint8_t *data = NULL;

    if (rx_len == expected_data + 2U && rx[1] == expected_data) {
        /* 标准应答：功能码 + 字节数 + 数据 */
        data = &rx[2];
    } else if (rx_len == expected_data + 1U) {
        /*
         * 兼容分支：部分国产水表省略"字节数"字段，直接跟数据。
         * 原实现按 len==8 处理的就是这种帧，必须保留以维持现场可用。
         */
        data = &rx[1];
        ESP_LOGW(TAG, "应答缺少字节数字段，按兼容模式解析");
    } else {
        set_fault(fault, MODBUS_FAULT_BAD_LENGTH);
        ESP_LOGW(TAG, "%s: 收到 %u, 期望 %u",
                 modbus_fault_name(MODBUS_FAULT_BAD_LENGTH),
                 (unsigned)rx_len, (unsigned)(expected_data + 2U));
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint16_t i = 0; i < count; i++) {
        values[i] = ((uint16_t)data[i * 2] << 8) | data[i * 2 + 1];
    }
    return ESP_OK;
}

esp_err_t modbus_write_single_coil(uint8_t slave, uint16_t coil, bool on,
                                   modbus_fault_t *fault)
{
    set_fault(fault, MODBUS_FAULT_NONE);

    const uint8_t pdu[5] = {
        MODBUS_FUNC_WRITE_SINGLE_COIL,
        (uint8_t)(coil >> 8), (uint8_t)(coil & 0xFF),
        on ? 0xFF : 0x00, 0x00,
    };

    uint8_t rx[MODBUS_MAX_PDU_LEN];
    size_t rx_len = 0;
    const esp_err_t err = modbus_transfer(slave, pdu, sizeof(pdu), rx, sizeof(rx),
                                          &rx_len, MODBUS_TIMEOUT_MS, fault);
    if (err != ESP_OK) {
        return err;
    }

    /* 标准应答是请求原样回显 */
    if (rx_len == sizeof(pdu) && memcmp(rx, pdu, sizeof(pdu)) == 0) {
        return ESP_OK;
    }
    /* 兼容分支：个别水表回显中地址字节不一致，只核对线圈值 */
    if (rx_len >= 5U && rx[3] == (on ? 0xFF : 0x00)) {
        ESP_LOGW(TAG, "线圈写应答非标准回显，已按线圈值匹配放行");
        return ESP_OK;
    }

    set_fault(fault, MODBUS_FAULT_BAD_FRAME);
    ESP_LOGW(TAG, "%s: 线圈写应答不匹配 (len=%u)",
             modbus_fault_name(MODBUS_FAULT_BAD_FRAME), (unsigned)rx_len);
    return ESP_ERR_INVALID_RESPONSE;
}
