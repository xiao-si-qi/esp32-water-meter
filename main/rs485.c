#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp_check.h"
#include "esp_log.h"

#include "rs485.h"

static const char *TAG = "rs485";

#define RS485_RX_RING_SIZE          512
#define RS485_TX_RING_SIZE          256
#define RS485_FIRST_BYTE_TIMEOUT_MS 300
#define RS485_INTER_BYTE_TIMEOUT_MS   5   /* 9600bps 下 3.5 字符约 4ms，取 5ms 判帧 */
#define RS485_DE_SETTLE_MS            2
#define RS485_MUTEX_TIMEOUT_MS     1000

static SemaphoreHandle_t s_bus_lock = NULL;

static inline void de_set_level(bool transmit)
{
    gpio_set_level(RS485_PIN_DE, transmit ? 1 : 0);
}

static esp_err_t rs485_do_exchange(const uint8_t *tx, size_t tx_len,
                                   uint8_t *rx, size_t rx_capacity, size_t *rx_len,
                                   uint32_t timeout_ms)
{
    /* 丢掉上一轮可能残留的字节，否则会被误当成应答首字节 */
    uart_flush_input(RS485_UART_PORT);

    de_set_level(true);

    const int written = uart_write_bytes(RS485_UART_PORT, tx, tx_len);
    if (written != (int)tx_len) {
        de_set_level(false);
        ESP_LOGE(TAG, "UART 发送失败: 期望 %u 字节, 实际 %d", (unsigned)tx_len, written);
        return ESP_FAIL;
    }

    uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(RS485_DE_SETTLE_MS));
    de_set_level(false);

    int received_bytes = uart_read_bytes(RS485_UART_PORT, rx, 1, pdMS_TO_TICKS(timeout_ms));
    if (received_bytes != 1) {
        *rx_len = 0;
        return ESP_ERR_TIMEOUT;
    }

    size_t received = 1U;
    while (received < rx_capacity) {
        received_bytes = uart_read_bytes(RS485_UART_PORT, rx + received,
                                         rx_capacity - received,
                                         pdMS_TO_TICKS(RS485_INTER_BYTE_TIMEOUT_MS));
        if (received_bytes <= 0) {
            break;
        }
        received += (size_t)received_bytes;
    }

    *rx_len = received;
    return ESP_OK;
}

esp_err_t rs485_init(void)
{
    if (s_bus_lock != NULL) {
        return ESP_OK;
    }

    s_bus_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_bus_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建总线互斥锁失败");

    const uart_config_t uart_cfg = {
        .baud_rate = RS485_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(RS485_UART_PORT, RS485_RX_RING_SIZE,
                                            RS485_TX_RING_SIZE, 0, NULL, 0),
                        TAG, "安装 UART 驱动失败");
    ESP_RETURN_ON_ERROR(uart_param_config(RS485_UART_PORT, &uart_cfg),
                        TAG, "配置 UART 参数失败");
    ESP_RETURN_ON_ERROR(uart_set_pin(RS485_UART_PORT, RS485_PIN_TX, RS485_PIN_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "绑定 UART 引脚失败");

    const gpio_config_t de_io_cfg = {
        .pin_bit_mask = 1ULL << RS485_PIN_DE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&de_io_cfg), TAG, "配置方向引脚失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(RS485_PIN_DE, 0), TAG, "初始化方向引脚电平失败");

    ESP_LOGI(TAG, "RS485 就绪 (UART%d, TX=%d, RX=%d, DE=%d, %d bps)",
             RS485_UART_PORT, RS485_PIN_TX, RS485_PIN_RX, RS485_PIN_DE, RS485_BAUD_RATE);
    return ESP_OK;
}

esp_err_t rs485_exchange(const uint8_t *tx, size_t tx_len,
                         uint8_t *rx, size_t rx_capacity, size_t *rx_len,
                         uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(tx != NULL && tx_len > 0, ESP_ERR_INVALID_ARG, TAG, "发送参数非法");
    ESP_RETURN_ON_FALSE(rx != NULL && rx_capacity > 0 && rx_len != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "接收参数非法");
    ESP_RETURN_ON_FALSE(s_bus_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "总线尚未初始化");

    *rx_len = 0;

    /* 串行化整次收发：报文之间不能交错，否则收发器方向会互相打架 */
    if (xSemaphoreTake(s_bus_lock, pdMS_TO_TICKS(timeout_ms + RS485_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "总线被长时间占用，放弃本次收发");
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t err = rs485_do_exchange(tx, tx_len, rx, rx_capacity, rx_len, timeout_ms);

    xSemaphoreGive(s_bus_lock);
    return err;
}
