/* common.h —— OSKit 公共工具：计时 / 统计 / 文件 / JSON / 错误报告 */
#ifndef OSKIT_COMMON_H
#define OSKIT_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define OSKIT_VERSION "0.1.0"

/* 统一错误输出前缀为 "oskit: ..."，die() 直接以退出码 1 结束 */
void die(const char *fmt, ...) __attribute__((noreturn));
void warnf(const char *fmt, ...);

/* CLOCK_MONOTONIC 纳秒时钟，全部基准测试的统一时钟源 */
uint64_t now_ns(void);

/* ---------- 样本统计：min / median / mean / p95 / max ---------- */
typedef struct {
    double *v;
    size_t  n, cap;
} samples;

typedef struct {
    size_t n;
    double min, max, mean, median, p95;
} stats_result;

/* s 需先清零；返回 0 成功，内存不足返回 -1 */
int  samples_push(samples *s, double x);
/* 样本数 < 1 返回 -1；结果写入 out（已排序副作用：会重排 s->v） */
int  stats_compute(samples *s, stats_result *out);

/* 一次性读入整个文件；成功返回 0，*buf 需 free()；失败返回 -1 并置 errno 相关信息 */
int xread_file(const char *path, void **buf, size_t *len);

/* 向 fp 输出 JSON 字符串字面量（含引号），转义 \" \\ \n \r \t \b \f 与 <0x20 */
void json_escape(FILE *fp, const char *s);

/* "--name" 匹配助手（含 NULL 安全） */
int arg_is(const char *arg, const char *longname);

#endif
