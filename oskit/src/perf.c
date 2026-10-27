/* perf.c —— 性能测试：11 项微基准 + 统计分布
 * 设计见 docs/01-perf.md：统计而非单点、先预热、防优化消除。 */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------- 基准输出模型：若干行(标签+单位+样本集) ---------- */
typedef struct {
    char     label[32];
    char     unit[16];
    samples  smp;
} bench_row;

typedef struct {
    bench_row *rows;
    size_t     nrow, cap;
} bench_out;

static bench_row *out_add_row(bench_out *o, const char *label, const char *unit)
{
    if (o->nrow == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 4;
        bench_row *nr = realloc(o->rows, nc * sizeof *nr);
        if (!nr) return NULL;
        o->rows = nr;
        o->cap = nc;
    }
    bench_row *r = &o->rows[o->nrow++];
    snprintf(r->label, sizeof r->label, "%s", label);
    snprintf(r->unit, sizeof r->unit, "%s", unit);
    memset(&r->smp, 0, sizeof r->smp);
    return r;
}

typedef struct {
    const char *name;
    const char *desc;
    int (*run)(int quick, bench_out *out); /* 0=成功 */
} bench_t;

/* ---------- 显示宽度对齐(把 CJK 按 2 列计) ---------- */
static size_t disp_width(const char *s)
{
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0xe0 && p[1] >= 0x80) { w += 2; if (*p >= 0xf0) w += 1; }
        else if (*p < 0x80) w += 1;
    return w;
}

static void print_padded(const char *s, size_t width)
{
    fputs(s, stdout);
    for (size_t i = disp_width(s); i < width; i++)
        putchar(' ');
}

/* ===================================================================== */
/* 1. cpu-int：寄存器整数运算（xorshift 混合），指标 Mops/s               */
/* ===================================================================== */
static volatile uint64_t g_sink;

static int bench_cpu_int(int quick, bench_out *out)
{
    bench_row *row = out_add_row(out, "", "Mops/s");
    if (!row) return -1;
    size_t iters = quick ? 8u << 20 : 64u << 20;
    int reps = quick ? 3 : 7;
    for (int r = 0; r < reps; r++) {
        uint64_t acc = 0x9e3779b97f4a7c15ull;
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < iters; i++) {
            acc ^= acc << 13;
            acc ^= acc >> 7;
            acc ^= acc << 17;
            acc += i;
        }
        uint64_t t1 = now_ns();
        g_sink += acc;
        samples_push(&row->smp, (double)iters / ((double)(t1 - t0) / 1e9) / 1e6);
    }
    return 0;
}

/* ===================================================================== */
/* 2. cpu-mem：memcpy 带宽分档（4K/64K/1M/16M → L1/L2/RAM）               */
/* ===================================================================== */
static int bench_cpu_mem(int quick, bench_out *out)
{
    static const size_t sizes[] = { 4 << 10, 64 << 10, 1 << 20, 16 << 20 };
    size_t target = quick ? 32u << 20 : 128u << 20; /* 每样本搬运总量 */
    int reps = quick ? 2 : 4;
    void *src = NULL, *dst = NULL;
    size_t biggest = sizes[sizeof sizes / sizeof sizes[0] - 1];
    src = malloc(biggest);
    dst = malloc(biggest);
    if (!src || !dst) { free(src); free(dst); return -1; }
    memset(src, 0x5a, biggest);

    int rc = 0;
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0] && rc == 0; si++) {
        size_t sz = sizes[si];
        char lab[16];
        if (sz >= (1 << 20)) snprintf(lab, sizeof lab, "%zuM", sz >> 20);
        else snprintf(lab, sizeof lab, "%zuK", sz >> 10);
        bench_row *row = out_add_row(out, lab, "MB/s");
        if (!row) { rc = -1; break; }
        size_t passes = target / sz < 1 ? 1 : target / sz;
        for (int r = 0; r < reps; r++) {
            uint64_t t0 = now_ns();
            for (size_t p = 0; p < passes; p++)
                memcpy(dst, src, sz);
            uint64_t t1 = now_ns();
            samples_push(&row->smp,
                (double)(passes * sz) / ((double)(t1 - t0) / 1e9) / (1024.0 * 1024.0));
        }
    }
    free(src);
    free(dst);
    return rc;
}

/* ===================================================================== */
/* 3. mem-latency：随机环指针追逐（依赖加载，预取失效），ns/次             */
/* ===================================================================== */
static int bench_mem_latency(int quick, bench_out *out)
{
    bench_row *row = out_add_row(out, "", "ns/次");
    if (!row) return -1;
    size_t n = quick ? 1u << 15 : 1u << 20;   /* 32K / 1M 节点 */
    size_t chase = quick ? 1u << 20 : 1u << 24;
    size_t *a = malloc(n * sizeof *a);
    if (!a) return -1;
    for (size_t i = 0; i < n; i++) a[i] = i;
    /* Sattolo 洗牌 → 恰好一个覆盖全数组的环 */
    srand(42);
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)rand() % i;
        size_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
    int reps = quick ? 2 : 5;
    for (int r = 0; r < reps; r++) {
        size_t idx = 0;
        uint64_t t0 = now_ns();
        for (size_t k = 0; k < chase; k++)
            idx = a[idx];
        uint64_t t1 = now_ns();
        g_sink += idx;
        samples_push(&row->smp, (double)(t1 - t0) / (double)chase);
    }
    free(a);
    return 0;
}

/* ===================================================================== */
/* 4. mem-alloc：malloc/free 小对象吞吐，万次/s                           */
/* ===================================================================== */
static int bench_mem_alloc(int quick, bench_out *out)
{
    bench_row *row = out_add_row(out, "", "万次/s");
    if (!row) return -1;
    size_t iters = quick ? 20000 : 100000;
    int reps = quick ? 2 : 5;
    for (int r = 0; r < reps; r++) {
        volatile char touch = 0;
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < iters; i++) {
            char *p = malloc(64);
            if (!p) { free(NULL); continue; }
            p[0] = (char)i;
            touch = p[0];
            free(p);
        }
        uint64_t t1 = now_ns();
        g_sink += touch;
        samples_push(&row->smp, (double)iters / ((double)(t1 - t0) / 1e9) / 10000.0);
    }
    return 0;
}

/* ===================================================================== */
/* 5. syscall：getpid() 热循环，ns/次（不走 vDSO 的真实陷入路径）          */
/* ===================================================================== */
static int bench_syscall(int quick, bench_out *out)
{
    bench_row *row = out_add_row(out, "", "ns/次");
    if (!row) return -1;
    size_t iters = quick ? 200000 : 1000000;
    int reps = quick ? 2 : 5;
    for (int r = 0; r < reps; r++) {
        long pid = 0;
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < iters; i++)
            pid += syscall(SYS_getpid);
        uint64_t t1 = now_ns();
        g_sink += (uint64_t)pid;
        samples_push(&row->smp, (double)(t1 - t0) / (double)iters);
    }
    return 0;
}

/* ===================================================================== */
/* 6. ctx-switch：父子进程管道乒乓，ns/往返（≈2 次调度切换）               */
/* ===================================================================== */
static int bench_ctx_switch(int quick, bench_out *out)
{
    bench_row *row = out_add_row(out, "", "ns/往返");
    if (!row) return -1;
    size_t rounds = quick ? 10000 : 100000;
    int reps = quick ? 2 : 5;
    int p2c[2], c2p[2];
    if (pipe(p2c) || pipe(c2p))
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* 子进程：读到 0 字节(父关写端)即退出 */
        close(p2c[1]);
        close(c2p[0]);
        char b;
        while (read(p2c[0], &b, 1) == 1) {
            if (write(c2p[1], &b, 1) != 1) break;
        }
        _exit(0);
    }
    close(p2c[0]);
    close(c2p[1]);
    int rc = 0;
    char b = 'x';
    for (int r = 0; r < reps && rc == 0; r++) {
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < rounds; i++) {
            if (write(p2c[1], &b, 1) != 1 || read(c2p[0], &b, 1) != 1) { rc = -1; break; }
        }
        uint64_t t1 = now_ns();
        if (rc == 0)
            samples_push(&row->smp, (double)(t1 - t0) / (double)rounds);
    }
    close(p2c[1]);
    close(c2p[0]);
    int st;
    waitpid(pid, &st, 0);
    return rc;
}

/* ===================================================================== */
/* 7. fork：fork + _exit + waitpid，µs/次                                 */
/* ===================================================================== */
static int bench_fork(int quick, bench_out *out)
{
    bench_row *row = out_add_row(out, "", "µs/次");
    if (!row) return -1;
    size_t iters = quick ? 50 : 200;
    int reps = quick ? 2 : 5;
    for (int r = 0; r < reps; r++) {
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < iters; i++) {
            pid_t p = fork();
            if (p == 0) _exit(0);
            int st;
            waitpid(p, &st, 0);
        }
        uint64_t t1 = now_ns();
        samples_push(&row->smp, (double)(t1 - t0) / (double)iters / 1000.0);
    }
    return 0;
}

/* ===================================================================== */
/* 8. thread：pthread_create + join，µs/次                                */
/* ===================================================================== */
static void *thread_nop(void *arg) { (void)arg; return NULL; }

static int bench_thread(int quick, bench_out *out)
{
    bench_row *row = out_add_row(out, "", "µs/次");
    if (!row) return -1;
    size_t iters = quick ? 1000 : 5000;
    int reps = quick ? 2 : 5;
    for (int r = 0; r < reps; r++) {
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < iters; i++) {
            pthread_t t;
            pthread_create(&t, NULL, thread_nop, NULL);
            pthread_join(t, NULL);
        }
        uint64_t t1 = now_ns();
        samples_push(&row->smp, (double)(t1 - t0) / (double)iters / 1000.0);
    }
    return 0;
}

/* ===================================================================== */
/* 9-11. 磁盘：顺序写 / 顺序读 / fsync                                    */
/* ===================================================================== */
static const char *fs_type_name(long t)
{
    switch ((unsigned long)t) {
    case 0x01021994: return "tmpfs";
    case 0xef53:     return "ext2/3/4";
    case 0x9123683e: return "btrfs";
    case 0x794c7630: return "overlayfs";
    case 0x09041934: return "anon_inodefs";
    case 0x6e736663: return "nsfs";
    case 0x73727279: return "9p(WSL 挂载, 结果≈内存速度)";
    default:         return "未知(按 statfs f_type 十六进制识别)";
    }
}

static void print_fs_note(void)
{
    struct statfs st;
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    if (statfs(dir, &st) == 0)
        printf("磁盘临时目录 %s (%s)\n", dir, fs_type_name(st.f_type));
}

static int bench_disk_rw(int quick, bench_out *out)
{
    bench_row *wrow = out_add_row(out, "write", "MB/s");
    bench_row *rrow = out_add_row(out, "read",  "MB/s");
    if (!wrow || !rrow) return -1;
    size_t total = quick ? 32u << 20 : 256u << 20;
    size_t chunk = 1u << 20;
    int reps = quick ? 2 : 3;

    char tmpl[] = "/tmp/oskit-perf-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        warnf("磁盘测试: 无法创建临时文件(%s): %s", tmpl, strerror(errno));
        return -1;
    }
    unlink(tmpl); /* 句柄仍有效, 退出即清, 异常也不留残文件 */
    char *buf = malloc(chunk);
    if (!buf) { close(fd); return -1; }
    memset(buf, 0xa5, chunk);
    int rc = 0;
    for (int r = 0; r < reps && rc == 0; r++) {
        if (ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) == (off_t)-1) { rc = -1; break; }
        uint64_t t0 = now_ns();
        for (size_t off = 0; off < total; off += chunk)
            if (write(fd, buf, chunk) != (ssize_t)chunk) { rc = -1; break; }
        uint64_t t1 = now_ns();
        if (rc == 0) {
            samples_push(&wrow->smp, (double)total / ((double)(t1 - t0) / 1e9) / (1024.0 * 1024.0));
            /* 读阶段：读的可能是页缓存(与真实回源有别, 见文档) */
            if (lseek(fd, 0, SEEK_SET) == (off_t)-1) { rc = -1; break; }
            uint64_t s = 0;
            t0 = now_ns();
            for (size_t off = 0; off < total; off += chunk)
                if (read(fd, buf, chunk) != (ssize_t)chunk) { rc = -1; break; }
                else s += buf[0];
            t1 = now_ns();
            g_sink += s;
            if (rc == 0)
                samples_push(&rrow->smp, (double)total / ((double)(t1 - t0) / 1e9) / (1024.0 * 1024.0));
        }
    }
    free(buf);
    close(fd);
    return rc;
}

static int bench_disk_fsync(int quick, bench_out *out)
{
    bench_row *row = out_add_row(out, "", "ms/次");
    if (!row) return -1;
    size_t iters = quick ? 50 : 200;
    char tmpl[] = "/tmp/oskit-perf-fsync-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    unlink(tmpl);
    char page[4096];
    memset(page, 0x33, sizeof page);
    int rc = 0;
    for (int r = 0; r < (quick ? 2 : 3) && rc == 0; r++) {
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < iters; i++) {
            if (pwrite(fd, page, sizeof page, (off_t)(i % 64) * 4096) != (ssize_t)sizeof page ||
                fsync(fd) != 0) { rc = -1; break; }
        }
        uint64_t t1 = now_ns();
        if (rc == 0)
            samples_push(&row->smp, (double)(t1 - t0) / (double)iters / 1e6);
    }
    close(fd);
    return rc;
}

/* ===================================================================== */
/* 注册表 / 运行器 / 输出                                                 */
/* ===================================================================== */
static const bench_t g_benches[] = {
    { "cpu-int",         "整数运算",       bench_cpu_int      },
    { "cpu-mem",         "内存拷贝带宽",   bench_cpu_mem      },
    { "mem-latency",     "访存延迟",       bench_mem_latency  },
    { "mem-alloc",       "堆分配吞吐",     bench_mem_alloc    },
    { "syscall",         "系统调用开销",   bench_syscall      },
    { "ctx-switch",      "进程切换",       bench_ctx_switch   },
    { "fork",            "进程创建回收",   bench_fork         },
    { "thread",          "线程创建回收",   bench_thread       },
    { "disk-seq",        "磁盘顺序读写",   bench_disk_rw      },
    { "disk-fsync",      "磁盘持久化延迟", bench_disk_fsync   },
};

#define NBENCH (sizeof g_benches / sizeof g_benches[0])

static void perf_usage(void)
{
    puts("用法: oskit perf [--list] [--quick] [--json] [基准名 ...]");
    puts("");
    puts("基准项:");
    for (size_t i = 0; i < NBENCH; i++)
        printf("  %-14s %s\n", g_benches[i].name, g_benches[i].desc);
    puts("");
    puts("  --quick  快速模式(样本少/负载小, 适合冒烟)");
    puts("  --json   输出 JSON");
    puts("  --list   仅列出基准项");
}

static void print_stat_line(const char *label, const char *unit, stats_result *st)
{
    print_padded(label, 30);
    print_padded(unit, 12);
    printf("n=%-3zu min %10.1f  中位 %10.1f  均值 %10.1f  p95 %10.1f  max %10.1f\n",
           st->n, st->min, st->median, st->mean, st->p95, st->max);
}

int perf_main(int argc, char **argv)
{
    int quick = 0, json = 0, list = 0;
    const char *want[NBENCH];
    size_t nwant = 0;

    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "--quick")) quick = 1;
        else if (arg_is(argv[i], "--json")) json = 1;
        else if (arg_is(argv[i], "--list")) list = 1;
        else if (arg_is(argv[i], "--help") || arg_is(argv[i], "-h")) { perf_usage(); return 0; }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "oskit perf: 未知选项 %s\n", argv[i]);
            return 2;
        } else {
            int found = 0;
            for (size_t k = 0; k < NBENCH; k++)
                if (strcmp(argv[i], g_benches[k].name) == 0) found = 1;
            if (!found) {
                fprintf(stderr, "oskit perf: 未知基准: %s (oskit perf --list)\n", argv[i]);
                return 2;
            }
            if (nwant < NBENCH) want[nwant++] = argv[i];
        }
    }
    if (list) {
        for (size_t i = 0; i < NBENCH; i++)
            printf("%-14s %s\n", g_benches[i].name, g_benches[i].desc);
        return 0;
    }

    if (!json) {
        printf("== OSKit 性能测试%s ==\n", quick ? " (quick)" : "");
        fflush(stdout);
    }
    int any_fail = 0;
    if (json) putchar('[');
    int first_bench = 1;

    for (size_t i = 0; i < NBENCH; i++) {
        if (nwant) {
            int sel = 0;
            for (size_t k = 0; k < nwant; k++)
                if (strcmp(want[k], g_benches[i].name) == 0) sel = 1;
            if (!sel) continue;
        }
        if (!strcmp(g_benches[i].name, "disk-seq") && !json && !nwant)
            print_fs_note();

        bench_out out = { 0 };
        /* 预热一次不计入(统计前提, 见 docs/01) */
        int rc = g_benches[i].run(1, &out);
        for (size_t k = 0; k < out.nrow; k++) free(out.rows[k].smp.v);
        free(out.rows);
        out = (bench_out){ 0 };

        rc = g_benches[i].run(quick, &out);
        if (rc != 0 || out.nrow == 0) {
            warnf("基准 %s 失败, 已跳过", g_benches[i].name);
            any_fail = 1;
            for (size_t k = 0; k < out.nrow; k++) free(out.rows[k].smp.v);
            free(out.rows);
            continue;
        }
        if (json) {
            if (!first_bench) putchar(',');
            first_bench = 0;
            printf("{\"name\":\"%s\",\"desc\":\"%s\",\"rows\":[", g_benches[i].name, g_benches[i].desc);
            for (size_t r = 0; r < out.nrow; r++) {
                bench_row *row = &out.rows[r];
                stats_result st;
                if (r) putchar(',');
                fputs("{\"label\":", stdout);
                json_escape(stdout, row->label);
                printf(",\"unit\":\"%s\",\"samples\":{", row->unit);
                if (stats_compute(&row->smp, &st) == 0)
                    printf("\"n\":%zu,\"min\":%.2f,\"median\":%.2f,\"mean\":%.2f,\"p95\":%.2f,\"max\":%.2f}}",
                           st.n, st.min, st.median, st.mean, st.p95, st.max);
                else
                    printf("}}");
            }
            fputs("]}", stdout);
        } else {
            for (size_t r = 0; r < out.nrow; r++) {
                bench_row *row = &out.rows[r];
                stats_result st;
                if (stats_compute(&row->smp, &st) != 0)
                    continue;
                char full[64];
                if (out.nrow > 1)
                    snprintf(full, sizeof full, "%s[%s]", g_benches[i].name, row->label);
                else
                    snprintf(full, sizeof full, "%s", g_benches[i].name);
                char lab[80];
                snprintf(lab, sizeof lab, "%s %s", full, g_benches[i].desc);
                print_stat_line(lab, row->unit, &st);
            }
            fflush(stdout);
        }
        for (size_t k = 0; k < out.nrow; k++) free(out.rows[k].smp.v);
        free(out.rows);
    }
    if (json) puts("]");
    return any_fail ? 1 : 0;
}
