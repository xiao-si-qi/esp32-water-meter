/**
 * @file battery.h
 * @brief 电池电压采样（ADC1_CH0 / GPIO0，分压后接入）。
 */
#ifndef BATTERY_H
#define BATTERY_H

#include "esp_err.h"

/** @brief 一次完整采样的原始数据与换算结果，用于标定与排查 */
typedef struct {
    int   raw_median;   /* 有效采样点的中位数（ADC 计数），抗离群值 */
    int   raw_min;      /* 有效采样点最小值，用于判断离散度 */
    int   raw_max;      /* 有效采样点最大值 */
    int   raw_count;    /* 有效采样点数（已剔除饱和/异常点） */
    int   pin_mv;       /* 换算出的引脚电压（未乘分压比） */
    float voltage_v;    /* 还原后的电池电压 */
    int   percent;      /* 估算电量百分比 */
} battery_measurement_t;

/** @brief 初始化 ADC 单元与校准（校准不可用时自动退化为线性换算）。 */
esp_err_t battery_init(void);

/**
 * @brief 立即进行一次采样，返回全部中间值。
 *
 * 与 battery_read() 的区别：不做异常告警，可被网页诊断接口反复调用，
 * 便于和万用表读数在同一时刻对照。
 */
esp_err_t battery_measure(battery_measurement_t *out);

/**
 * @brief 采样电池电压并估算电量百分比。
 *
 * 内部多次采样取均值以抑制抖动，并在离散度/跳变异常时告警。
 */
esp_err_t battery_read(float *voltage_v, int *percent);

#endif /* BATTERY_H */
