/**
 * @file web_server.h
 * @brief 设备本地网页：状态看板 + 阀门控制 + Wi-Fi/MQTT 配置。
 *
 * 仅监听局域网，使用设备 IP 直接访问（http://<设备IP>/）。
 * 所有接口均为无状态 JSON，页面自身负责轮询刷新。
 */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

/** @brief 启动 HTTP 服务器。失败不影响设备其它功能。 */
esp_err_t web_server_start(void);

/** @brief 停止 HTTP 服务器。 */
void web_server_stop(void);

#endif /* WEB_SERVER_H */
