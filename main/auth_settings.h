/**
 * @file auth_settings.h
 * @brief 网页登录凭据（HTTP Basic Auth）。
 *
 * 默认 admin/admin，存在 NVS 里，可在网页上修改。
 * 注意：Basic Auth 只是 base64 编码，不是加密；局域网内可被嗅探。
 * 当前定位是"防止同一局域网内误操作"，不是对抗性安全。
 */
#ifndef AUTH_SETTINGS_H
#define AUTH_SETTINGS_H

#include "esp_err.h"

#define AUTH_SETTINGS_USER_MAX 33
#define AUTH_SETTINGS_PASS_MAX 65

typedef struct {
    char username[AUTH_SETTINGS_USER_MAX];
    char password[AUTH_SETTINGS_PASS_MAX];
} auth_settings_t;

/** @brief 载入凭据：优先 NVS，缺失则用 admin/admin。 */
esp_err_t auth_settings_load(auth_settings_t *out);

/** @brief 写入 NVS。用户名与密码均不能为空。 */
esp_err_t auth_settings_save(const auth_settings_t *settings);

#endif /* AUTH_SETTINGS_H */
