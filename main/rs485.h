/**
 * @file rs485.h
 * @brief RS485 半双工总线收发驱动（UART + 方向控制引脚）。
 *
 * 只负责“把一帧字节可靠地发出去、再收回来”，不理解 Modbus 语义。
 * 总线互斥锁在模块内部持有，因此多个任务可安全调用，不会互相打断报文。
 */
#ifndef RS485_H
#define RS485_H

#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

#define RS485_UART_PORT       UART_NUM_1
#define RS485_PIN_TX          6
#define RS485_PIN_RX          7
#define RS485_PIN_DE          3   /* 收发切换：1=发送，0=接收 */
#define RS485_BAUD_RATE       9600

/** @brief 安装 UART 驱动、配置引脚与总线互斥锁。可重复调用。 */
esp_err_t rs485_init(void);

/**
 * @brief 发送一帧并接收应答。
 *
 * 发送完成后等待最后一个字节移出移位寄存器再切回接收态，避免首字节被自身吞掉。
 * 接收侧按“首字节超时 + 帧内字节间隔”收敛整帧，不依赖调用方预先知道长度。
 *
 * @param tx            待发送数据
 * @param tx_len        发送长度
 * @param rx            接收缓冲区
 * @param rx_capacity   接收缓冲区容量
 * @param rx_len        输出：实际收到的字节数
 * @param timeout_ms    等待首字节的超时时间
 * @return ESP_OK           收到至少 1 字节
 * @return ESP_ERR_TIMEOUT  从站无应答
 */
esp_err_t rs485_exchange(const uint8_t *tx, size_t tx_len,
                         uint8_t *rx, size_t rx_capacity, size_t *rx_len,
                         uint32_t timeout_ms);

#endif /* RS485_H */
