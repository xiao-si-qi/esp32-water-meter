#include "esp_check.h"
#include "esp_log.h"

#include "modbus_rtu.h"
#include "rs485.h"
#include "water_meter.h"

static const char *TAG = "meter";

/* ------------------------- 该型号水表的寄存器映射 ------------------------- */
#define WM_SLAVE_ADDRESS      1
#define WM_REG_VOLUME         0x0000
#define WM_REG_VOLUME_COUNT   2      /* 32 位累计水量，高字在前 */
#define WM_COIL_VALVE         0x0001
#define WM_VOLUME_DIVISOR     100.0f /* 原始值 / 100 = m³ */

/*
 * 现场这台水表的 0x01 读线圈应答是非标准的，原实现按"帧长 8 字节、取第 6 字节
 * 等于 0xFF 判定开阀"来解析并一直工作正常，说明它回的是"地址 + 值"回显式帧
 * 而不是标准的"字节数 + 位图"。两种格式都要认，否则换表或换固件就会误判。
 *
 * 帧格式已在现场确认，默认关闭原始 PDU 打印以减少日志噪音；
 * 换表型号或怀疑解析异常时再置 1。
 */
#define WM_LOG_RAW_FRAMES     0

/* 临时诊断：启动时扫描 0x0000~0x000F 寄存器，用于确认水量到底映射在哪个地址。
 * 排查结束后置 0，重新置 1 可在下次启动时打印寄存器表。 */
#define WM_DIAG_SCAN_REGISTERS 0

#if WM_DIAG_SCAN_REGISTERS
static void water_meter_diag_scan(void)
{
    ESP_LOGW(TAG, "=== 诊断：逐个读取寄存器 0x0000-0x000F ===");
    for (uint16_t address = 0x0000; address < 0x0010; address += 2) {
        uint16_t registers[2] = {0};
        modbus_fault_t fault = MODBUS_FAULT_NONE;
        if (modbus_read_holding_registers(WM_SLAVE_ADDRESS, address, 2, registers, &fault) == ESP_OK) {
            ESP_LOGW(TAG, "  0x%04X: %5u (0x%04X) | 0x%04X: %5u (0x%04X)",
                     address, registers[0], registers[0],
                     address + 1, registers[1], registers[1]);
        } else {
            ESP_LOGW(TAG, "  0x%04X: 读取失败 - %s", address, modbus_fault_name(fault));
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ESP_LOGW(TAG, "=== 诊断扫描结束 ===");
}
#endif

esp_err_t water_meter_init(void)
{
    /* 物理总线由本层独占：上层只调用语义化的读水量 / 读写阀门接口 */
    ESP_RETURN_ON_ERROR(rs485_init(), TAG, "RS485 总线初始化失败");

    ESP_LOGI(TAG, "从站地址=%d, 水量寄存器=0x%04X x%d, 阀门线圈=0x%04X",
             WM_SLAVE_ADDRESS, WM_REG_VOLUME, WM_REG_VOLUME_COUNT, WM_COIL_VALVE);

#if WM_DIAG_SCAN_REGISTERS
    water_meter_diag_scan();
#endif

    return ESP_OK;
}

esp_err_t water_meter_read_volume(float *volume_m3, modbus_fault_t *fault)
{
    ESP_RETURN_ON_FALSE(volume_m3 != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");

    uint16_t registers[WM_REG_VOLUME_COUNT] = {0};
    const esp_err_t err = modbus_read_holding_registers(WM_SLAVE_ADDRESS, WM_REG_VOLUME,
                                                        WM_REG_VOLUME_COUNT, registers, fault);
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t raw = ((uint32_t)registers[0] << 16) | (uint32_t)registers[1];
    *volume_m3 = (float)raw / WM_VOLUME_DIVISOR;
    /* 遥测每轮已打印「累计水量」，这里降为 debug，避免每分钟重复两条 */
    ESP_LOGD(TAG, "水量原始寄存器: 0x%04X 0x%04X -> 原始值 %lu -> %.2f m³",
             registers[0], registers[1], (unsigned long)raw, *volume_m3);
    return ESP_OK;
}

esp_err_t water_meter_read_valve(bool *is_open, modbus_fault_t *fault)
{
    ESP_RETURN_ON_FALSE(is_open != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");

    const uint8_t pdu[5] = {
        MODBUS_FUNC_READ_COILS,
        (uint8_t)(WM_COIL_VALVE >> 8), (uint8_t)(WM_COIL_VALVE & 0xFF),
        0x00, 0x01,                       /* 读 1 个线圈 */
    };

    uint8_t rx[MODBUS_MAX_PDU_LEN];
    size_t rx_len = 0;
    const esp_err_t err = modbus_transfer(WM_SLAVE_ADDRESS, pdu, sizeof(pdu),
                                          rx, sizeof(rx), &rx_len, MODBUS_TIMEOUT_MS, fault);
    if (err != ESP_OK) {
        return err;
    }

#if WM_LOG_RAW_FRAMES
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, rx, rx_len, ESP_LOG_INFO);
#endif

    if (rx_len == 3U && rx[1] == 0x01) {
        /* 标准应答：功能码 + 字节数(1) + 位图 */
        *is_open = (rx[2] & 0x01) != 0;
    } else if (rx_len == 4U && rx[1] == 0x02) {
        /* 兼容：字节数声明为 2，取低字节 */
        *is_open = (rx[3] == 0xFF) || ((rx[3] & 0x01) != 0);
    } else if (rx_len == 5U) {
        /* 兼容：回显式应答 [功能码][地址Hi][地址Lo][值Hi][值Lo] */
        *is_open = (rx[4] == 0xFF) || (rx[3] == 0xFF);
    } else {
        if (fault != NULL) {
            *fault = MODBUS_FAULT_BAD_LENGTH;
        }
        ESP_LOGW(TAG, "%s: 阀门应答无法识别 (len=%u)",
                 modbus_fault_name(MODBUS_FAULT_BAD_LENGTH), (unsigned)rx_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t water_meter_write_valve(bool open, modbus_fault_t *fault)
{
    ESP_LOGI(TAG, "下发阀门指令: %s", open ? "开 (0x05 -> 0xFF00)" : "关 (0x05 -> 0x0000)");
    return modbus_write_single_coil(WM_SLAVE_ADDRESS, WM_COIL_VALVE, open, fault);
}
