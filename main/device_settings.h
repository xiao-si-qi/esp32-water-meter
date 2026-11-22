/**
 * @file device_settings.h
 * @brief 设备中文显示名（与 MQTT 主题里的设备 ID 分开）。
 *
 * 设备 ID（wm_001）决定主题和实体 id，编译期固定；
 * 中文名只用于 HA 界面显示，可在网页里随时改，存在 NVS。
 */
#ifndef DEVICE_SETTINGS_H
#define DEVICE_SETTINGS_H

#include <stddef.h>

#include "esp_err.h"

#define DEVICE_NAME_MAX 32

/** @brief 取中文名；NVS 没有则返回 app_config.h 里的默认值。 */
const char *device_settings_name(void);

/** @brief 保存中文名（不能为空）。 */
esp_err_t device_settings_set_name(const char *name);

#endif /* DEVICE_SETTINGS_H */
