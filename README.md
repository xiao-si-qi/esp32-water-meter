# Aqueduct · ESP32-C3 超声水表抄表器

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Switchyard](https://img.shields.io/badge/上位机-switchyard-41BDF5.svg)](https://github.com/xiao-si-qi/switchyard)

> aqueduct：引水渠。铁路调车归 [Switchyard](https://github.com/xiao-si-qi/switchyard)，
> 家里这条"水渠"归它管。

一块 **ESP32-C3 + RS485** 的超声水表抄表器：读累计水量、开关阀门、量电池与温湿度、
检测漏水与通信故障，通过 MQTT 接入 Home Assistant / Switchyard。

```
        RS485 (Modbus RTU)              Wi-Fi + MQTT
超声水表 ──────────────► ESP32-C3 ──────────────────► switchyard ──► HA
  · 累计水量                                        · 设备台账 / 实体
  · 阀门开关     AHT10 温湿度                        · 远程 OTA
  · 到位反馈     电池 ADC / 漏水探头                  · 日志回传
```

## 功能

| 能力（caps）| 说明 |
|---|---|
| `water` | 累计水量（m³），周期抄表 |
| `valve` | 阀门开关，带行程等待与状态确认，异步不阻塞 |
| `battery` / `battery_v` | 电量百分比 + 电池电压 |
| `climate` | AHT10 温度 / 湿度 |
| `leak` | 漏水探头（去抖 5 次，选脚避开 strapping） |
| `comm_fault` | RS485 通信故障告警 |
| `fw` `rssi` `uptime` `event` `log` `diag` | 版本、信号、运行时长、事件、按需日志、错误摘要 |

内置本地网页（`http://<设备IP>/`，默认 `admin/admin`）：状态看板、阀门控制、
WiFi/MQTT 配置（存 NVS，运行时可切明文/TLS）、固件上传（网页 OTA）。

## 硬件

| 项 | 型号 / 引脚 |
|---|---|
| MCU | ESP32-C3（4 MB flash，双 OTA 槽） |
| RS485 | UART + DE 方向控制（引脚见 `main/app_config.h`） |
| 水表 | 超声水表，Modbus RTU 从站（寄存器映射隔离在 `water_meter.c`，换表只改这一层） |
| 温湿度 | AHT10（I2C） |
| 漏水 | `GPIO1` —— **必须避开 `GPIO2/8/9`** 这三个 strapping 脚 |
| 阀门/按钮/LED | 开阀、关阀、状态灯、MQTT 活动灯（引脚见 `app_config.h`） |

## 软件结构

依赖方向严格单向，下层不知道上层存在：

```
main.c                 只做初始化编排，无业务逻辑
├── wifi_manager       Wi-Fi STA / 非阻塞退避重连 + 触发 SNTP 对时
├── mqtt_manager       MQTT 连接（明文/TLS 双支持）、switchyard 协议、语义化上报
│   ├── mqtt_settings  连接参数的 NVS 覆盖层（运行时可切换 mqtt/mqtts）
│   └── time_sync      SNTP 对时（TLS 证书校验的前提）
├── telemetry          周期抄表任务（水量 / 阀门 / 电池 / RSSI）
├── web_server         本地网页：状态看板 / 阀门控制 / 配置 / 固件更新
│   └── index.html     内嵌前端（EMBED_FILES，离线可用）
├── valve              阀门控制任务（指令队列 + 行程等待 + 状态确认）
│   └── water_meter    水表语义层（寄存器映射、量纲换算、帧格式兼容）
│       └── modbus_rtu Modbus RTU 协议（组帧、CRC、异常码）
│           └── rs485  RS485 收发（UART + DE 方向控制 + 总线互斥）
├── battery            电池 ADC 采样
├── aht10_sensor       温湿度
├── leak_sensor        漏水检测（去抖）
├── leds               全部指示灯（状态 / MQTT 活动 / 阀位）
├── buttons            按键消抖 → 投递阀门请求
├── log_buffer         最近 N 行日志环形缓冲（供 `log` 主题按需回传）
└── app_state          受互斥锁保护的全局状态快照（唯一共享状态）
```

| 文件 | 职责 |
|---|---|
| `app_config.h` | 全部编译期参数：Topic、引脚、周期、任务栈/优先级 |
| `app_state.c/h` | 唯一跨任务共享状态，互斥锁 + 快照读取 |
| `rs485.c/h` | 字节级收发，内部持有总线锁，多任务安全 |
| `modbus_rtu.c/h` | 与设备无关的 Modbus 主站 |
| `water_meter.c/h` | 该型号水表的寄存器/量纲/帧格式兼容 |
| `valve.c/h` | 阀门状态机，异步化 |
| `telemetry.c/h` | 周期采集与上报 |
| `mqtt_manager.c/h` | 连接 + 表驱动的 HA 发现 + 语义化上报；按 URI 自动选明文/TLS |
| `mqtt_settings.c/h` / `wifi_settings.c/h` | MQTT / WiFi 参数的 NVS 覆盖层（网页可改） |
| `web_server.c/h` / `index.html` | HTTP 服务与单文件前端（无外部依赖） |
| `auth_settings.c/h` | 网页登录凭据（NVS，默认 `admin/admin`） |
| `time_sync.c/h` | SNTP 对时，为 TLS 提供可信时间 |
| `ota_remote.c/h` | 从 switchyard 拉 manifest 与固件（HTTP OTA） |
| `log_buffer.c/h` | 环形日志缓冲 |

## 几个踩过的坑

- **TLS 前必须对时**：ESP32 上电是 1970 年，任何证书都判"尚未生效"，
  `mqtts://` 握手必然失败。所以 `time_sync_is_valid()` 为真之前不建 TLS。
- **漏水脚不能随便选**：`GPIO2/8/9` 是 strapping 脚，漏水时被拉低、恰好这时断电重启，
  设备就进下载模式起不来。最终用 `GPIO1`。
- **阀门是异步的**：行程要几秒，不能让事件任务阻塞，否则 MQTT 心跳会断；
  阀门任务维护状态机，指令进队列。
- **RS485 总线要加锁**：抄表和阀门指令共用一条总线，字节级加锁避免帧交错。

## 接入 Switchyard / Home Assistant

协议见 [switchyard/docs/PROTOCOL.md](https://github.com/xiao-si-qi/switchyard/blob/main/docs/PROTOCOL.md)

- 设备 → `switchyard/<id>/{state,availability,meta,event,diag,log}`
- 上位机 → `switchyard/<id>/{cmd,cfg}`（`cmd` 带单调 `seq`，设备去重）
- `meta.caps` 声明能力，插件只生成声明过的实体；能力减少时自动清理孤儿实体
- 固件和布局不走 MQTT：设备拿 URL 自己用 HTTP 拉

## 编译

需要 **ESP-IDF v5.5**（实测 v5.5.4）。

```bash
# 1) 填凭据（不进仓库）
cp main/secrets.h.example main/secrets.h
#   编辑 main/secrets.h：WiFi、MQTT、OTA 根地址

# 2) 编译与烧写（首次走 USB）
idf.py set-target esp32c3
idf.py build
idf.py -p <串口> flash monitor
```

不建 `secrets.h` 也能编译，只会用 `CHANGE_ME` 占位符；此时可通过设备网页填 WiFi/MQTT。

### 之后的升级

运行后再也不用插 USB：

- **switchyard 面板**：上传 `.bin` → 下发 OTA → 设备自己拉取并重启
- **设备网页**：打开 `http://<设备IP>/` 直接上传 `.bin`

两种方式都写入另一个 OTA 槽，失败可回滚。

## 分区表（4 MB flash）

```
nvs        0x9000    24 KB
otadata    0xf000     8 KB
phy_init   0x11000    4 KB
ota_0      0x20000    1.7 MB
ota_1      0x1d0000   1.7 MB
```

## 许可

MIT，见 [LICENSE](LICENSE)。
