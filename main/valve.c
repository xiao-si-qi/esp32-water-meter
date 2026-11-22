#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "esp_check.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_state.h"
#include "valve.h"
#include "water_meter.h"

static const char *TAG = "valve";

typedef struct {
    valve_source_t source;
    bool           open;
} valve_command_t;

static QueueHandle_t s_request_queue = NULL;
static valve_state_listener_t s_listener = NULL;
static valve_event_listener_t s_event_listener = NULL;

static const char *valve_source_name(valve_source_t source)
{
    switch (source) {
    case VALVE_SOURCE_BUTTON:
        return "本地按键";
    case VALVE_SOURCE_WEB:
        return "网页";
    case VALVE_SOURCE_MQTT:
        return "HA/MQTT";
    case VALVE_SOURCE_LEAK:
        return "漏水联动";
    default:
        return "未知";
    }
}

/* 只由阀门任务写，其它任务只读，故用 volatile 保证可见性 */
static volatile bool s_is_open = false;
static volatile bool s_is_moving = false;

static void notify_listener(void)
{
    if (s_listener != NULL) {
        s_listener(s_is_open, s_is_moving);
    }
}

static void report_event(const char *event, bool fault_active)
{
    ESP_LOGW(TAG, "事件上报: %s (fault=%d)", event, fault_active ? 1 : 0);

    /* 同步进共享状态，网页与 MQTT 读同一份，不会出现两处不一致 */
    app_state_set_valve_fault(fault_active);
    app_state_set_last_event(event);

    if (s_event_listener != NULL) {
        s_event_listener(event, fault_active);
    }
}

static void sync_state_to_center(bool target_open)
{
    app_state_set_valve(s_is_open, s_is_moving, target_open);
    notify_listener();
}

/**
 * @brief 确认结果：区分"机械没到位"和"根本读不到"，两者排查方向完全不同
 */
typedef enum {
    CONFIRM_OK = 0,
    CONFIRM_MISMATCH,    /* 读到了，但状态与目标不符 —— 机械问题 */
    CONFIRM_COMM_FAIL,   /* 一次都没读到 —— 通信问题 */
} confirm_result_t;

/**
 * @brief 动作完成后确认阀门真实状态。
 *
 * 机械阀需要时间，所以先等 VALVE_TRAVEL_TIME_MS 再轮询；
 * 轮询期间总线由 rs485 内部互斥，遥测任务不会被卡死。
 */
static confirm_result_t confirm_valve_state(bool target_open)
{
    int successful_reads = 0;

    for (int attempt = 1; attempt <= VALVE_CONFIRM_RETRIES; attempt++) {
        bool actual = false;
        modbus_fault_t fault = MODBUS_FAULT_NONE;

        if (water_meter_read_valve(&actual, &fault) == ESP_OK) {
            successful_reads++;
            s_is_open = actual;   /* 记录最近一次真实读数，失败时不会留下乐观值 */
            ESP_LOGI(TAG, "状态确认 %d/%d: %s", attempt, VALVE_CONFIRM_RETRIES, actual ? "开" : "关");
            if (actual == target_open) {
                return CONFIRM_OK;
            }
        } else {
            ESP_LOGW(TAG, "状态读取失败 %d/%d: %s", attempt, VALVE_CONFIRM_RETRIES,
                     modbus_fault_name(fault));
        }
        vTaskDelay(pdMS_TO_TICKS(VALVE_CONFIRM_INTERVAL_MS));
    }

    return (successful_reads > 0) ? CONFIRM_MISMATCH : CONFIRM_COMM_FAIL;
}

static void valve_task(void *arg)
{
    (void)arg;

    valve_command_t command;
    for (;;) {
        if (xQueueReceive(s_request_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const bool target_open = command.open;
        const char *source = valve_source_name(command.source);
        ESP_LOGI(TAG, "处理%s指令（来源：%s）", target_open ? "开阀" : "关阀", source);

        /*
         * 漏水期间拒绝一切开阀请求（漏水联动自身只会关阀，不受影响）。
         * 这样即使有人在 HA 里误点开阀，也不会在漏水未排除时把水放进来。
         */
        if (target_open && app_state_leak_detected()) {
            ESP_LOGW(TAG, "检测到漏水，拒绝开阀指令（来源：%s），请先排除漏水", source);
            report_event("valve_blocked_by_leak", false);
            continue;
        }

        /*
         * 以水表实际状态为准，不信任本地缓存。
         * 缓存只在上电时为默认值，若期间阀门被别处（面板/手动）改动过，
         * 直接用缓存判断"已处于目标状态"会把指令静默吞掉 —— 对阀门来说是事故。
         */
        bool actual_open = s_is_open;
        modbus_fault_t read_fault = MODBUS_FAULT_NONE;
        if (water_meter_read_valve(&actual_open, &read_fault) == ESP_OK) {
            if (actual_open != s_is_open) {
                ESP_LOGI(TAG, "同步阀门实际状态: %s", actual_open ? "开" : "关");
                s_is_open = actual_open;
                app_state_set_valve(s_is_open, false, s_is_open);
            }
        } else {
            /* 读不到状态时宁可多执行一次动作，也不能丢弃指令 */
            ESP_LOGW(TAG, "无法读取阀门实际状态(%s)，仍按指令执行",
                     modbus_fault_name(read_fault));
        }

        if (s_is_open == target_open) {
            ESP_LOGI(TAG, "阀门已处于%s状态，忽略指令", target_open ? "开" : "关");
            continue;
        }

        s_is_moving = true;
        sync_state_to_center(target_open);

        modbus_fault_t write_fault = MODBUS_FAULT_NONE;
        const esp_err_t err = water_meter_write_valve(target_open, &write_fault);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "阀门指令下发失败: %s", modbus_fault_name(write_fault));
            app_state_set_comm_fail(true);
            s_is_moving = false;
            sync_state_to_center(target_open);
            report_event("valve_cmd_failed", true);
            continue;
        }

        ESP_LOGI(TAG, "指令已确认，等待 %d ms 机械动作", VALVE_TRAVEL_TIME_MS);

        /*
         * 乐观更新：水表已回显确认收到指令，此刻就把目标状态上报出去，
         * 让 HA / 网页上的开关立刻翻转，而不是等 12 秒机械动作完成后才变。
         * 若最终确认失败，下面的收尾逻辑会用真实状态再上报一次把它纠正回来。
         */
        s_is_open = target_open;
        sync_state_to_center(target_open);

        vTaskDelay(pdMS_TO_TICKS(VALVE_TRAVEL_TIME_MS));

        switch (confirm_valve_state(target_open)) {
        case CONFIRM_OK:
            app_state_set_comm_fail(false);
            report_event("valve_ok", false);
            break;

        case CONFIRM_MISMATCH:
            /* 读得到状态，只是没到位：机械卡阻或行程不足，不是通信问题 */
            ESP_LOGW(TAG, "阀门未到位，当前记录为: %s", s_is_open ? "开" : "关");
            report_event("valve_not_confirmed", true);
            break;

        case CONFIRM_COMM_FAIL:
        default: {
            /* 一次都没读到：属于通信问题，故障传感器也要置位 */
            modbus_fault_t fault = MODBUS_FAULT_NONE;
            bool actual = false;
            if (water_meter_read_valve(&actual, &fault) == ESP_OK) {
                s_is_open = actual;
            }
            app_state_set_comm_fail(true);
            ESP_LOGW(TAG, "阀门状态无法确认: %s", modbus_fault_name(fault));
            report_event("valve_comm_lost", true);
            break;
        }
        }

        gpio_set_level(PIN_VALVE, s_is_open ? 1 : 0);

        s_is_moving = false;
        sync_state_to_center(s_is_open);
        ESP_LOGI(TAG, "阀门流程结束，当前状态: %s", s_is_open ? "开" : "关");
    }
}

esp_err_t valve_init(valve_state_listener_t state_listener,
                     valve_event_listener_t event_listener)
{
    if (s_request_queue != NULL) {
        return ESP_OK;
    }
    s_listener = state_listener;
    s_event_listener = event_listener;

    const gpio_config_t valve_io_cfg = {
        .pin_bit_mask = 1ULL << PIN_VALVE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&valve_io_cfg), TAG, "配置阀门输出引脚失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_VALVE, 0), TAG, "初始化阀门输出电平失败");

    s_request_queue = xQueueCreate(VALVE_QUEUE_LENGTH, sizeof(valve_command_t));
    ESP_RETURN_ON_FALSE(s_request_queue != NULL, ESP_ERR_NO_MEM, TAG, "创建阀门指令队列失败");

    const BaseType_t created = xTaskCreate(valve_task, "valve", TASK_STACK_VALVE, NULL,
                                           TASK_PRIO_VALVE, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "创建阀门任务失败");

    /* 上电先与水表对一次真实状态，避免默认值与现场不符 */
    bool initial_open = false;
    modbus_fault_t init_fault = MODBUS_FAULT_NONE;
    if (water_meter_read_valve(&initial_open, &init_fault) == ESP_OK) {
        s_is_open = initial_open;
        ESP_LOGI(TAG, "阀门初始状态: %s", initial_open ? "开" : "关");
    } else {
        ESP_LOGW(TAG, "读取阀门初始状态失败(%s)，暂按关处理",
                 modbus_fault_name(init_fault));
    }

    app_state_set_valve(s_is_open, false, s_is_open);
    ESP_LOGI(TAG, "阀门模块就绪 (GPIO%d, 行程 %d ms)", PIN_VALVE, VALVE_TRAVEL_TIME_MS);
    return ESP_OK;
}

esp_err_t valve_request(valve_source_t source, bool open)
{
    ESP_RETURN_ON_FALSE(s_request_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "阀门模块未初始化");

    const valve_command_t command = {.source = source, .open = open};

    /*
     * 队列长度 1 + 覆盖写入 = "最新指令生效"。
     * 用 xQueueOverwrite 而不是 xQueueSend：机械动作期间到达的指令不会因为
     * 队列已满而被丢弃，也不会让调用方（按键/HTTP/MQTT 回调）阻塞或报错。
     */
    if (xQueueOverwrite(s_request_queue, &command) != pdTRUE) {
        ESP_LOGW(TAG, "投递阀门指令失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "收到%s请求（来源：%s）", open ? "开阀" : "关阀",
             valve_source_name(source));
    return ESP_OK;
}
