/**
 * @file log_buffer.c
 * @brief 日志环形缓冲实现。
 *
 * 钩子由 esp_log 在**每个任务**里调用，所以这里必须：
 *   - 不阻塞（用自旋锁，不用可能睡眠的 mutex）；
 *   - 不做内存分配、不打日志（否则递归）；
 *   - 尽快返回，真正的网络发送交给上报任务。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "log_buffer.h"

#define LOG_RING_LINES   40
#define LOG_LINE_MAX     120
#define LOG_LEVEL_PREFIX_LEN 1

static char s_ring[LOG_RING_LINES][LOG_LINE_MAX];
static int s_head = 0;          /* 下一个写入位置 */
static int s_count = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_hook = NULL;

static char s_diag_line[LOG_LINE_MAX];
static volatile bool s_diag_pending = false;

/** 去掉结尾换行和 ANSI 转义，日志行才有可读性。 */
static void trim_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    /* esp_log 默认不带颜色；带 ESC 时直接截断到 ESC 之前 */
    char *esc = strchr(line, 0x1b);
    if (esc != NULL) {
        *esc = '\0';
    }
}

static int log_hook(const char *fmt, va_list args)
{
    char line[LOG_LINE_MAX];

    /* 先格式化一份副本，不能消耗原 args */
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n > 0) {
        trim_line(line);

        portENTER_CRITICAL(&s_lock);
        snprintf(s_ring[s_head], LOG_LINE_MAX, "%s", line);
        s_head = (s_head + 1) % LOG_RING_LINES;
        if (s_count < LOG_RING_LINES) {
            s_count++;
        }
        /* 只记住"第一条"待上报的错误，避免刷屏覆盖掉根因 */
        if (!s_diag_pending && (line[0] == 'E' || line[0] == 'W')) {
            snprintf(s_diag_line, sizeof(s_diag_line), "%s", line);
            s_diag_pending = true;
        }
        portEXIT_CRITICAL(&s_lock);
    }

    if (s_prev_hook != NULL) {
        return s_prev_hook(fmt, args);
    }
    return vprintf(fmt, args);
}

void log_buffer_init(void)
{
    if (s_prev_hook != NULL) {
        return;
    }
    s_prev_hook = esp_log_set_vprintf(log_hook);
}

size_t log_buffer_dump(char *out, size_t len, int lines)
{
    if (out == NULL || len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (lines <= 0 || lines > LOG_RING_LINES) {
        lines = LOG_RING_LINES;
    }

    portENTER_CRITICAL(&s_lock);
    const int total = (s_count < lines) ? s_count : lines;
    /* s_head 是下一个写入位，最旧的一行在 s_head - total 处 */
    int start = (s_head - total + LOG_RING_LINES) % LOG_RING_LINES;
    size_t used = 0;
    for (int i = 0; i < total; i++) {
        const char *row = s_ring[(start + i) % LOG_RING_LINES];
        const size_t need = strlen(row) + 1;   /* +1 给换行 */
        if (used + need + 1 >= len) {
            break;
        }
        const size_t copy = strlen(row);
        memcpy(out + used, row, copy);
        used += copy;
        out[used++] = '\n';
        out[used] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);

    return used;
}

bool log_buffer_take_diag(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return false;
    }
    out[0] = '\0';

    portENTER_CRITICAL(&s_lock);
    if (!s_diag_pending) {
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    s_diag_pending = false;
    char line[LOG_LINE_MAX];
    snprintf(line, sizeof(line), "%s", s_diag_line);
    portEXIT_CRITICAL(&s_lock);

    /* JSON 里需要转义的字符：仅处理 " 和 \，够覆盖日志文本 */
    /* JSON 里只保留前 96 字节：日志行可能是长中文，整体塞进去会被截断成非法 JSON */
    line[96] = '\0';
    char escaped[LOG_LINE_MAX * 2];
    size_t j = 0;
    for (size_t i = 0; line[i] != '\0' && j + 2 < sizeof(escaped); i++) {
        if (line[i] == '"' || line[i] == '\\') {
            escaped[j++] = '\\';
        }
        escaped[j++] = line[i];
    }
    escaped[j] = '\0';

    snprintf(out, len,
             "{\"level\":\"%c\",\"line\":\"%s\",\"heap\":%d,\"uptime\":%lld}",
             (line[0] != '\0') ? line[0] : 'E',
             escaped,
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (long long)(esp_timer_get_time() / 1000000));
    return true;
}
