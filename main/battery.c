#include <limits.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

#include "battery.h"

static const char *TAG = "battery";

/* ------------------------- 硬件相关参数 ------------------------- */
#define BATTERY_ADC_UNIT          ADC_UNIT_1
#define BATTERY_ADC_CHANNEL       ADC_CHANNEL_0     /* GPIO0 */
#define BATTERY_ADC_ATTEN         ADC_ATTEN_DB_12

/*
 * 分压比：万用表实测 Vbat=3.940 V、Vpin=2.607 V（测量时充电已停止），
 * 比值 1.5113。原值 1.5 按标称电阻估算，实测偏差 0.75%（约 29 mV）。
 */
#define BATTERY_DIVIDER_RATIO     1.511f

/*
 * ADC 单点校准系数，默认不校正。
 *
 * 曾按一次"ADC 反推 2676 mV、万用表实测 2607 mV"算出 0.974 做校正，但随后的
 * 测量又回到 2596 mV（与万用表仅差 0.4%），说明那次是离群值。单点校正不可靠，
 * 故保持 1.0。若确需校正，用 /api/battery 的多次读数取中位数与万用表比对。
 */
#define BATTERY_ADC_GAIN          1.0f

/* 电量估算改用查表（见 s_lipo_curve），不再使用线性的空满电压 */
/*
 * 实测发现：分压电路源阻抗偏高，ADC 采样保持电容在采样窗口内充不满，
 * 单次采样点在 2.3~3.3 V 之间乱跳（离散度最高近 1000 计数），个别点打满 4095。
 * 因此改为：多采 + 剔除饱和点 + 取中位数，而不是取算术平均。
 * 作用是压住离群值（平均会被打满的样本拉高，曾导致读数虚高到 4.26 V）。
 * 治本仍需硬件上加 0.1 µF 滤波电容或降低分压电阻阻值。
 */
#define BATTERY_SAMPLE_COUNT      16
#define BATTERY_SAMPLE_INTERVAL_MS  5
#define BATTERY_ADC_FULL_SCALE_MV 3300
#define BATTERY_ADC_SATURATED     4090   /* >= 此值视为饱和，直接丢弃 */

/*
 * 单节锂电池空载电压 - 电量对照表（分段线性插值）。
 *
 * 之前用 (V - 3.3) / (4.2 - 3.3) 线性映射，但锂电池放电曲线不是线性的：
 * 3.7~3.85 V 这一段很平坦（对应 20%~70%），线性公式会**高估**中段电量。
 * 实测 3.806 V 时线性算 56%，实际约 45%，偏差 11%。
 */
typedef struct {
    int mv;
    int percent;
} lipo_curve_point_t;

static const lipo_curve_point_t s_lipo_curve[] = {
    {4200, 100},
    {4100,  90},
    {4000,  80},
    {3930,  70},
    {3870,  60},
    {3820,  50},
    {3790,  40},
    {3770,  30},
    {3740,  20},
    {3680,  10},
    {3450,   5},
    {3000,   0},
};

/** @brief 按空载电压查表插值出电量百分比 */
static int lipo_voltage_to_percent(int battery_mv)
{
    const size_t count = sizeof(s_lipo_curve) / sizeof(s_lipo_curve[0]);

    if (battery_mv >= s_lipo_curve[0].mv) {
        return 100;
    }
    if (battery_mv <= s_lipo_curve[count - 1].mv) {
        return 0;
    }

    for (size_t i = 0; i + 1 < count; i++) {
        const int hi_mv = s_lipo_curve[i].mv;
        const int lo_mv = s_lipo_curve[i + 1].mv;
        if (battery_mv <= hi_mv && battery_mv >= lo_mv) {
            const int span = hi_mv - lo_mv;
            if (span <= 0) {
                return s_lipo_curve[i].percent;
            }
            const int offset = battery_mv - lo_mv;
            const int hi_pct = s_lipo_curve[i].percent;
            const int lo_pct = s_lipo_curve[i + 1].percent;
            return lo_pct + (hi_pct - lo_pct) * offset / span;
        }
    }
    return 0;
}

/*
 * 电量平滑。
 *
 * 锂电池平台区（3.74~3.87 V）电压变化很小而电量变化很大：查表每 30 mV 就是 10%，
 * 因此电压只要有 15 mV 的漂移，显示的百分比就会跳 5%。
 * 实测遇到过"充电后表面电荷消散导致 3.85→3.75 V 掉 90 mV"，百分比一次性从
 * 56% 跳到 20% —— 数字本身没错，但观感上像是故障。
 *
 * 处理：对"用于查表的电压"做 EMA（而不是直接查瞬时值），并加 2% 迟滞抑制抖动。
 * 注意上报的 voltage_v 仍是真实瞬时值，不做平滑。
 */
#define BATTERY_PERCENT_EMA_ALPHA  0.4f
#define BATTERY_PERCENT_HYSTERESIS 2

static float s_percent_voltage = -1.0f;
static int s_last_percent = -1;

static int battery_smoothed_percent(float voltage_v)
{
    if (s_percent_voltage < 0.0f) {
        s_percent_voltage = voltage_v;          /* 首次直接采用 */
    } else {
        s_percent_voltage = s_percent_voltage * (1.0f - BATTERY_PERCENT_EMA_ALPHA) +
                            voltage_v * BATTERY_PERCENT_EMA_ALPHA;
    }

    const int battery_mv = (int)(s_percent_voltage * 1000.0f);
    int percent = lipo_voltage_to_percent(battery_mv);

    if (s_last_percent >= 0 && abs(percent - s_last_percent) < BATTERY_PERCENT_HYSTERESIS) {
        percent = s_last_percent;
    }
    s_last_percent = percent;
    return percent;
}

/*
 * 噪声是突发性的：单轮 16 点中位数仍会被整轮污染带偏（实测出现过 4.2 V）。
 * 因此做"多轮 + 中位数的中位数"：每轮独立取中位数，再对各轮中位数取中位数。
 * 只有当一半以上的轮次同时被污染时才会出错，抗突发噪声能力强得多。
 */
#define BATTERY_ROUNDS             5

static int compare_int(const void *lhs, const void *rhs)
{
    const int a = *(const int *)lhs;
    const int b = *(const int *)rhs;
    return (a > b) - (a < b);
}

/*
 * 异常阈值，只在超过时告警：
 *   1) 单次采样内的离散度大  -> 源阻抗过高 / 缺滤波电容 / 干扰
 *   2) 两次采样之间数值跳变  -> 接触不良 / 充电状态变化 / 电池自身
 */
#define BATTERY_ADC_SPREAD_WARN     120   /* 采样点 max-min 计数差 */
#define BATTERY_JUMP_WARN_V        0.15f  /* 相邻两次测量电压差（V） */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;

esp_err_t battery_init(void)
{
    if (s_adc_handle != NULL) {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle),
                        TAG, "创建 ADC 单元失败");

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg),
                        TAG, "配置 ADC 通道失败");

    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    const esp_err_t cali_err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (cali_err != ESP_OK) {
        /* 原实现用 ESP_ERROR_CHECK 直接 abort，这里降级为线性换算，保证设备仍能工作 */
        s_cali_handle = NULL;
        ESP_LOGW(TAG, "ADC 校准不可用 (%s)，改用线性换算", esp_err_to_name(cali_err));
    }

    ESP_LOGI(TAG, "电池采样就绪 (ADC1_CH0 / GPIO0, 分压比 %.3f)", BATTERY_DIVIDER_RATIO);
    return ESP_OK;
}

esp_err_t battery_measure(battery_measurement_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "输出指针为空");
    ESP_RETURN_ON_FALSE(s_adc_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "电池采样未初始化");

    int round_medians[BATTERY_ROUNDS];
    int rounds_ok = 0;
    int total_valid = 0;
    int global_min = INT_MAX;
    int global_max = INT_MIN;

    for (int round = 0; round < BATTERY_ROUNDS; round++) {
        int samples[BATTERY_SAMPLE_COUNT];
        int count = 0;

        for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
            int sample = 0;
            ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &sample),
                                TAG, "ADC 采样失败");
            vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_INTERVAL_MS));
            if (sample >= BATTERY_ADC_SATURATED) {
                continue;   /* 饱和点没有信息量，丢弃 */
            }
            samples[count++] = sample;
        }

        if (count < 3) {
            ESP_LOGW(TAG, "第 %d 轮有效点不足 (%d/%d)，本轮丢弃",
                     round + 1, count, BATTERY_SAMPLE_COUNT);
            continue;
        }

        qsort(samples, (size_t)count, sizeof(int), compare_int);
        round_medians[rounds_ok++] = samples[count / 2];
        total_valid += count;
        if (samples[0] < global_min) {
            global_min = samples[0];
        }
        if (samples[count - 1] > global_max) {
            global_max = samples[count - 1];
        }
    }

    ESP_RETURN_ON_FALSE(rounds_ok > 0, ESP_ERR_INVALID_STATE, TAG,
                        "所有采样轮次均被干扰，检查 ADC 输入端");

    /* 对各轮中位数再取中位数 */
    qsort(round_medians, (size_t)rounds_ok, sizeof(int), compare_int);

    out->raw_count = total_valid;
    out->raw_min = global_min;
    out->raw_max = global_max;
    out->raw_median = round_medians[rounds_ok / 2];

    if (rounds_ok < BATTERY_ROUNDS) {
        ESP_LOGW(TAG, "%d/%d 轮被干扰丢弃，结果仍按剩余轮次计算",
                 BATTERY_ROUNDS - rounds_ok, BATTERY_ROUNDS);
    }

    int pin_mv = 0;
    if (s_cali_handle != NULL) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali_handle, out->raw_median, &pin_mv),
                            TAG, "ADC 电压换算失败");
    } else {
        pin_mv = (int)(((int64_t)out->raw_median * BATTERY_ADC_FULL_SCALE_MV) / 4095);
    }
    out->pin_mv = pin_mv;

    const float calibrated_mv = (float)pin_mv * BATTERY_ADC_GAIN;
    out->voltage_v = calibrated_mv * BATTERY_DIVIDER_RATIO / 1000.0f;

    /* 电量按锂电池放电曲线查表（电压先做 EMA 平滑，避免百分比随电压漂移跳动） */
    out->percent = battery_smoothed_percent(out->voltage_v);

    return ESP_OK;
}

esp_err_t battery_read(float *voltage_v, int *percent)
{
    ESP_RETURN_ON_FALSE(voltage_v != NULL && percent != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "输出指针为空");

    battery_measurement_t m;
    ESP_RETURN_ON_ERROR(battery_measure(&m), TAG, "电池采样失败");

    *voltage_v = m.voltage_v;
    *percent = m.percent;

    ESP_LOGD(TAG, "ADC min/med/max=%d/%d/%d (有效 %d 点), 引脚=%d mV, 电池=%.3f V, %d%%",
             m.raw_min, m.raw_median, m.raw_max, m.raw_count,
             m.pin_mv, m.voltage_v, m.percent);

    /* ---- 硬件排查辅助：只在异常时告警 ---- */
    static float s_last_voltage = -1.0f;

    const int spread = m.raw_max - m.raw_min;
    if (spread > BATTERY_ADC_SPREAD_WARN) {
        ESP_LOGW(TAG, "采样离散度偏大: min=%d max=%d 差=%d（约 %d mV 引脚电压），"
                      "检查分压电阻阻值/滤波电容是否缺失，或 Wi-Fi 发射干扰",
                 m.raw_min, m.raw_max, spread,
                 (int)((int64_t)spread * BATTERY_ADC_FULL_SCALE_MV / 4095));
    }

    if (s_last_voltage > 0.0f) {
        const float delta = m.voltage_v - s_last_voltage;
        const float abs_delta = (delta < 0.0f) ? -delta : delta;
        if (abs_delta > BATTERY_JUMP_WARN_V) {
            ESP_LOGW(TAG, "两次测量跳变 %.3f V（上次 %.3f → 本次 %.3f），"
                          "若此时正在充电属正常，否则检查电池接触与分压电路",
                     abs_delta, s_last_voltage, m.voltage_v);
        }
    }
    s_last_voltage = m.voltage_v;

    return ESP_OK;
}
