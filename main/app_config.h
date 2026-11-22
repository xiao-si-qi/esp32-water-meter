/**
 * @file app_config.h
 * @brief 全部编译期可调参数集中于此。
 *
 * 约定：业务代码中不允许出现魔法数字，任何需要调整的数值都应定义在本文件。
 * 下一阶段接入网页配置时，Wi-Fi / MQTT 相关宏将由 NVS 覆盖，届时只需改这里。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ========================== 凭据（不进仓库） ========================== */
/*
 * WiFi / MQTT 密码、OTA 根地址都不写在源码里：真实值放 main/secrets.h
 * （已 .gitignore），仓库里只有 secrets.h.example 占位模板。
 *
 * 没建 secrets.h 也能编译，只是会用 "CHANGE_ME" 占位符 —— 设备连不上网，
 * 但可以通过设备网页（http://<设备IP>/）现场改 WiFi/MQTT，值存 NVS。
 */
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef APP_SECRET_WIFI_SSID
#define APP_SECRET_WIFI_SSID        "CHANGE_ME"
#define APP_SECRET_WIFI_PASSWORD    "CHANGE_ME"
#define APP_SECRET_MQTT_URI         "mqtt://mqtt.example.com:1883"
#define APP_SECRET_MQTT_USERNAME    "mqtt"
#define APP_SECRET_MQTT_PASSWORD    "CHANGE_ME"
#define APP_SECRET_OTA_BASE_URL     "https://ha.example.com"
#endif

/* ============================== Wi-Fi ============================== */
#define APP_WIFI_SSID                APP_SECRET_WIFI_SSID
#define APP_WIFI_PASSWORD            APP_SECRET_WIFI_PASSWORD
#define APP_WIFI_RETRY_MAX           5      /* 连续失败次数达到此值后重启 Wi-Fi 驱动 */
#define APP_WIFI_RETRY_BASE_MS       1000   /* 重连退避基准，按 2^n 递增 */
#define APP_WIFI_RETRY_CAP_MS        10000  /* 退避上限 */

/* =============================== MQTT =============================== */
/*
 * 同时支持明文与加密两种传输，由 URI 前缀决定：
 *   mqtt://host:1883    明文（开发阶段默认）
 *   mqtts://host:8883   TLS 加密（生产环境）
 * 运行时可用 mqtt_settings_save() 写 NVS 覆盖，无需重新编译。
 */
#define APP_MQTT_BROKER_URI          APP_SECRET_MQTT_URI

#define APP_MQTT_USERNAME            APP_SECRET_MQTT_USERNAME
#define APP_MQTT_PASSWORD            APP_SECRET_MQTT_PASSWORD
#define APP_MQTT_CLIENT_ID_PREFIX    "esp32_wm_"
#define APP_MQTT_KEEPALIVE_S         60

/* ============== 时间同步（MQTTS 证书校验的前提条件） ============== */
/*
 * ESP32 上电时系统时间是 1970 年，此时任何证书都会被判定为“尚未生效”，
 * TLS 握手必然失败。因此启用 mqtts:// 前必须先完成 SNTP 对时，
 * mqtt_manager 会等到 time_sync_is_valid() 为真后才建立 TLS 连接。
 */
#define APP_SNTP_SERVER_PRIMARY      "ntp.aliyun.com"
#define APP_SNTP_SERVER_FALLBACK     "203.107.6.88"   /* 阿里云 NTP 的 IP，DNS 故障时兜底 */
#define APP_SNTP_SERVER_TERTIARY     "cn.pool.ntp.org"
#define APP_TIME_ZONE                "CST-8"
#define APP_TIME_MIN_VALID_EPOCH     1704067200L      /* 2024-01-01 00:00:00 UTC */

/* Topic 前缀带 ha/，与原有版本保持一致，改动会导致 HA 实体断链 */
#define APP_MQTT_TOPIC_PREFIX        "ha/watermeter"
#define APP_MQTT_TOPIC_AVAILABILITY  APP_MQTT_TOPIC_PREFIX "/status"

#define APP_MQTT_TOPIC_WATER         APP_MQTT_TOPIC_PREFIX "/sensor/water"
#define APP_MQTT_TOPIC_IP            APP_MQTT_TOPIC_PREFIX "/sensor/ip"
#define APP_MQTT_TOPIC_RSSI          APP_MQTT_TOPIC_PREFIX "/sensor/rssi"
#define APP_MQTT_TOPIC_BATTERY_VOLT  APP_MQTT_TOPIC_PREFIX "/sensor/battery_voltage"
#define APP_MQTT_TOPIC_BATTERY_PCT   APP_MQTT_TOPIC_PREFIX "/sensor/battery_level"
#define APP_MQTT_TOPIC_TEMPERATURE   APP_MQTT_TOPIC_PREFIX "/sensor/temperature"
#define APP_MQTT_TOPIC_HUMIDITY      APP_MQTT_TOPIC_PREFIX "/sensor/humidity"
#define APP_MQTT_TOPIC_VALVE_STATE   APP_MQTT_TOPIC_PREFIX "/switch/valve/state"
#define APP_MQTT_TOPIC_VALVE_SET     APP_MQTT_TOPIC_PREFIX "/switch/valve/set"

/* 故障与事件上报：供 HA 自动化触发通知 */
#define APP_MQTT_TOPIC_EVENT         APP_MQTT_TOPIC_PREFIX "/event"
#define APP_MQTT_TOPIC_VALVE_FAULT   APP_MQTT_TOPIC_PREFIX "/binary/valve_fault"
#define APP_MQTT_TOPIC_COMM_FAULT    APP_MQTT_TOPIC_PREFIX "/binary/comm_fault"
#define APP_MQTT_TOPIC_LEAK          APP_MQTT_TOPIC_PREFIX "/binary/leak"

/* ====================== Home Assistant 自动发现 ====================== */
#define HA_DISCOVERY_PREFIX          "homeassistant"
#define APP_DEVICE_NAME              "入户水表"
#define APP_DEVICE_ID                "wm_001"     /* 参与 unique_id 拼接，不可随意修改 */
#define APP_DEVICE_MANUFACTURER      "Espressif"
#define APP_DEVICE_MODEL             "ESP32-C3-MAX485"

/* ========================= 引脚（ESP32-C3） ========================= */
#define PIN_VALVE                    10
#define PIN_STATUS_LED               13
#define PIN_MQTT_LED                 12
#define PIN_BUTTON_OPEN               8
#define PIN_BUTTON_CLOSE              9
#define PIN_LED_VALVE_OPEN           19
#define PIN_LED_VALVE_CLOSE          18

/*
 * 漏水检测输入。
 * 必须避开 strapping 引脚（GPIO2 / GPIO8 / GPIO9）：上电瞬间被拉低会进下载模式，
 * 若正好在漏水（引脚被拉低）时断电重启，设备就起不来了。
 * GPIO1 无任何特殊功能，是当前板上唯一安全可用的空闲脚。
 */
#define PIN_LEAK_SENSOR               1

/* ============================ 漏水检测 ============================ */
#define LEAK_POLL_MS                100   /* 采样周期 */
#define LEAK_DEBOUNCE_SAMPLES         5   /* 连续 5 次（500 ms）一致才认定 */
#define TASK_STACK_LEAK            2560
#define TASK_PRIO_LEAK                6   /* 高于阀门任务，保证告警及时 */

/* ========================== 周期与节拍 ========================== */
#define LED_TICK_MS                  50     /* LED 任务节拍 */
#define LED_MQTT_ACTIVITY_MS        100     /* MQTT 活动灯点亮时长 */
#define BUTTON_POLL_MS               10
#define BUTTON_DEBOUNCE_SAMPLES       3     /* 连续 N 次采样一致才认定状态翻转 */
#define TELEMETRY_PERIOD_MS       60000     /* 抄表周期 */
#define CLIMATE_PERIOD_MS         60000     /* 温湿度采样周期 */

/* ============================== 阀门 ============================== */
#define VALVE_TRAVEL_TIME_MS      12000     /* 水表机械阀动作时间，指令下发后需等待 */
#define VALVE_CONFIRM_RETRIES         5
#define VALVE_CONFIRM_INTERVAL_MS  2000
#define VALVE_QUEUE_LENGTH            1     /* 只保留最近一条指令 */

/* ============================ 任务参数 ============================ */
#define TASK_STACK_LEDS            2560
#define TASK_PRIO_LEDS                2

#define TASK_STACK_BUTTONS         2560
#define TASK_PRIO_BUTTONS             3

#define TASK_STACK_TELEMETRY       4096
#define TASK_PRIO_TELEMETRY           4

#define TASK_STACK_VALVE           3072
#define TASK_PRIO_VALVE               5

#define TASK_STACK_CLIMATE         4096
#define TASK_PRIO_CLIMATE             3

#define TASK_STACK_MQTT_SUPERVISOR 3072
#define TASK_PRIO_MQTT_SUPERVISOR     6

/* ==================== switchyard 设备协议（统一上位机） ====================
 * 设备只维护一条状态主题（retained JSON），由 switchyard 解析并生成 HA 实体，
 * 取代原先十几条 ha/watermeter/... 主题。
 */
#define APP_SY_DEVICE_ID             APP_DEVICE_ID      /* wm_001，只能是 a-z 0-9 - _ */
#define APP_SY_TOPIC_PREFIX          "switchyard/" APP_SY_DEVICE_ID
#define APP_SY_TOPIC_STATE           APP_SY_TOPIC_PREFIX "/state"
#define APP_SY_TOPIC_AVAILABILITY    APP_SY_TOPIC_PREFIX "/availability"
#define APP_SY_TOPIC_EVENT           APP_SY_TOPIC_PREFIX "/event"
#define APP_SY_TOPIC_CMD             APP_SY_TOPIC_PREFIX "/cmd"
#define APP_SY_TOPIC_META            APP_SY_TOPIC_PREFIX "/meta"
#define APP_SY_TOPIC_DIAG            APP_SY_TOPIC_PREFIX "/diag"   /* 错误摘要（retained，限流）*/
#define APP_SY_TOPIC_LOG             APP_SY_TOPIC_PREFIX "/log"    /* 按需抓取的日志（非 retained）*/
#define APP_SY_TYPE                  "meter"
#define APP_SY_CAPS                  "\"fw\",\"rssi\",\"uptime\",\"battery\",\"battery_v\"," \
                                     "\"water\",\"climate\",\"leak\",\"comm_fault\",\"online\"," \
                                     "\"valve\",\"ota\",\"reboot\",\"log\""
#define APP_SY_STATE_PERIOD_S        60      /* 周期上报（状态变化时也会立刻发） */
#define APP_SY_NVS_NAMESPACE         "switchyard"

/* OTA 拉取模式：switchyard 加载项里配的 public_url */
#define APP_OTA_BASE_URL             APP_SECRET_OTA_BASE_URL
#define APP_OTA_CHECK_INTERVAL_S     1800    /* 每 30 分钟自查一次 manifest */
#define APP_DIAG_MIN_INTERVAL_MS     60000   /* 错误摘要最快 1 分钟一条，避免刷屏 */

#endif /* APP_CONFIG_H */
