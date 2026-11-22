/**
 * @file ota_remote.h
 * @brief MQTT 触发的远程 OTA（switchyard 协议）。
 *
 * 两条升级路径：
 *   1. 推送：switchyard 下发 {"cmd":"ota","url":"...","version":"x"}
 *            → ota_remote_start() 直接下载该地址；
 *   2. 拉取：设备定期 GET <base>/local/switchyard/<id>/firmware.json
 *            → 版本不同则自动下载（断电/离线也不怕漏升级）。
 *
 * 下载在独立任务里跑，不阻塞 MQTT 事件任务；进度通过 mqtt_publish_event()
 * 以 {"ev":"ota_progress","pct":N} 的形式上报。
 */
#ifndef OTA_REMOTE_H
#define OTA_REMOTE_H

#include <stdbool.h>

#include "esp_err.h"

/** @brief 是否正在升级（升级期间禁止再次触发）。 */
bool ota_remote_busy(void);

/**
 * @brief 后台下载并写入非活动分区，成功后切换启动分区并重启。
 *
 * @param url     固件 .bin 的完整地址（http:// 或 https://）
 * @param version 目标版本号，仅用于日志与上报
 * @return ESP_OK 表示任务已启动（真正结果看 ota_progress / 重启）
 */
esp_err_t ota_remote_start(const char *url, const char *version);

/**
 * @brief 拉取模式：查 manifest，有新版本就自动升级。
 *
 * 需要 app_config.h 里的 APP_OTA_BASE_URL 配置正确；未配置时直接返回。
 */
void ota_remote_check_manifest(void);

#endif /* OTA_REMOTE_H */
