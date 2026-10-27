/* common.c —— 公共工具实现 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void die(const char *fmt, ...)
{
    va_list ap;
    fputs("oskit: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void warnf(const char *fmt, ...)
{
    va_list ap;
    fputs("oskit: 警告: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int samples_push(samples *s, double x)
{
    if (s->n == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 16;
        double *nv = realloc(s->v, ncap * sizeof(double));
        if (!nv)
            return -1;
        s->v = nv;
        s->cap = ncap;
    }
    s->v[s->n++] = x;
    return 0;
}

static int dbl_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int stats_compute(samples *s, stats_result *out)
{
    if (s->n == 0)
        return -1;
    qsort(s->v, s->n, sizeof(double), dbl_cmp);
    double sum = 0;
    for (size_t i = 0; i < s->n; i++)
        sum += s->v[i];
    out->n      = s->n;
    out->min    = s->v[0];
    out->max    = s->v[s->n - 1];
    out->mean   = sum / (double)s->n;
    out->median = (s->n & 1) ? s->v[s->n / 2]
                             : 0.5 * (s->v[s->n / 2 - 1] + s->v[s->n / 2]);
    size_t p95i = (size_t)((s->n * 95 + 99) / 100); /* ceil(0.95n) - 1 + 1，再夹取 */
    if (p95i > s->n) p95i = s->n;
    if (p95i == 0)   p95i = 1;
    out->p95 = s->v[p95i - 1];
    return 0;
}

int xread_file(const char *path, void **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    char *p = malloc(sz ? (size_t)sz : 1);
    if (!p) { fclose(f); errno = ENOMEM; return -1; }
    if (sz > 0 && fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p);
        fclose(f);
        return -1;
    }
    fclose(f);
    *buf = p;
    *len = (size_t)sz;
    return 0;
}

void json_escape(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n",  fp); break;
        case '\r': fputs("\\r",  fp); break;
        case '\t': fputs("\\t",  fp); break;
        case '\b': fputs("\\b",  fp); break;
        case '\f': fputs("\\f",  fp); break;
        default:
            if (*p < 0x20)
                fprintf(fp, "\\u%04x", *p);
            else
                fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

int arg_is(const char *arg, const char *longname)
{
    if (!arg)
        return 0;
    return strcmp(arg, longname) == 0;
}
