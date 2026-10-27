/* analyze.c —— 指令级分析与安全检查实现 */
#include "analyze.h"

#include <stdlib.h>
#include <string.h>

/* ---------- 符号提取 ---------- */
typedef struct {
    rv_sym *tab;
    size_t  n, cap;
} symcol;

static int col_cb(void *ud, const char *name, uint64_t addr, uint64_t size,
                  uint8_t info, uint16_t shndx)
{
    if (shndx == 0)
        return 0;
    uint8_t type = info & 0xf;
    if (type != 2 && type != 1 && type != 10)   /* FUNC/OBJECT/IFUNC */
        return 0;
    symcol *c = ud;
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 256;
        rv_sym *t = realloc(c->tab, nc * sizeof *t);
        if (!t) return 1;
        c->tab = t;
        c->cap = nc;
    }
    snprintf(c->tab[c->n].name, sizeof c->tab[0].name, "%s", name);
    c->tab[c->n].addr = addr;
    c->tab[c->n].size = size;
    c->n++;
    return 0;
}

static int sym_cmp(const void *a, const void *b)
{
    const rv_sym *x = a, *y = b;
    if (x->addr < y->addr) return -1;
    if (x->addr > y->addr) return 1;
    return 0;
}

int analyze_collect_syms(elf_file *ef, rv_sym **out, size_t *n)
{
    symcol c = { 0 };
    elf_for_each_sym(ef, col_cb, &c);
    if (c.tab)
        qsort(c.tab, c.n, sizeof *c.tab, sym_cmp);
    *out = c.tab;
    *n = c.n;
    return 0;
}

/* ---------- 运行画像 ---------- */
static void bar(FILE *out, double pct)
{
    int w = (int)(pct * 24.0 + 0.5);
    if (w > 24) w = 24;
    fputc('[', out);
    for (int i = 0; i < 24; i++) fputc(i < w ? '#' : '.', out);
    fputc(']', out);
}

void analyze_report(rv_vm *v, FILE *out, int json)
{
    double tot = (double)v->instret;
    if (tot <= 0) tot = 1;

    /* 热点 Top5: 简单选择排序前 5 */
    int idx[5] = { -1, -1, -1, -1, -1 };
    if (v->syms && v->sym_count) {
        for (int k = 0; k < 5; k++) {
            int best = -1;
            for (size_t i = 0; i < v->n_syms; i++) {
                if (v->sym_count[i] == 0) continue;
                int dup = 0;
                for (int j = 0; j < k; j++)
                    if (idx[j] == (int)i) dup = 1;
                if (dup) continue;
                if (best < 0 || v->sym_count[i] > v->sym_count[best])
                    best = (int)i;
            }
            idx[k] = best;
        }
    }

    if (json) {
        fprintf(out, "{\"instructions\":%llu,\"op_mix\":{",
                (unsigned long long)v->instret);
        for (int c = 0; c < RV_OP_CLASS_N; c++)
            fprintf(out, "%s\"%s\":%llu", c ? "," : "", rv_op_class_name(c),
                    (unsigned long long)v->op_count[c]);
        fprintf(out, "},\"branch\":{\"total\":%llu,\"taken\":%llu,\"taken_pct\":%.2f},"
                     "\"loads\":%llu,\"stores\":%llu,\"hot_symbols\":[",
                (unsigned long long)v->br_total, (unsigned long long)v->br_taken,
                v->br_total ? 100.0 * (double)v->br_taken / (double)v->br_total : 0.0,
                (unsigned long long)v->load_cnt, (unsigned long long)v->store_cnt);
        int f = 0;
        for (int k = 0; k < 5; k++) {
            if (idx[k] < 0) break;
            fprintf(out, "%s{\"name\":\"%s\",\"count\":%llu,\"pct\":%.2f}",
                    f++ ? "," : "", v->syms[idx[k]].name,
                    (unsigned long long)v->sym_count[idx[k]],
                    100.0 * (double)v->sym_count[idx[k]] / tot);
        }
        fputs("]}\n", out);
        return;
    }

    fprintf(out, "== 运行画像 (%llu 条指令) ==\n", (unsigned long long)v->instret);
    for (int c = 0; c < RV_OP_CLASS_N; c++) {
        double pct = 100.0 * (double)v->op_count[c] / tot;
        if (v->op_count[c] == 0) continue;
        fprintf(out, "  %-7s %10llu  %5.1f%% ", rv_op_class_name(c),
                (unsigned long long)v->op_count[c], pct);
        bar(out, pct / 100.0);
        fputc('\n', out);
    }
    fprintf(out, "  分支命中 %llu/%llu (%.1f%%) · load %llu · store %llu\n",
            (unsigned long long)v->br_taken, (unsigned long long)v->br_total,
            v->br_total ? 100.0 * (double)v->br_taken / (double)v->br_total : 0.0,
            (unsigned long long)v->load_cnt, (unsigned long long)v->store_cnt);
    if (idx[0] >= 0) {
        fprintf(out, "  热点符号:\n");
        for (int k = 0; k < 5 && idx[k] >= 0; k++)
            fprintf(out, "    %d. %-24s %8llu  %5.1f%%\n", k + 1,
                    v->syms[idx[k]].name, (unsigned long long)v->sym_count[idx[k]],
                    100.0 * (double)v->sym_count[idx[k]] / tot);
    } else if (v->instret) {
        fprintf(out, "  (无符号表, 热点归因不可用)\n");
    }
}

/* ---------- 加固检查(规则源自 ../oskit/src/vuln.c) ---------- */
#define PT_GNU_STACK_ 0x6474e551
#define PT_GNU_RELRO_ 0x6474e552
#define DT_BIND_NOW_  24
#define DT_FLAGS_     30
#define DT_FLAGS_1_   0x6ffffffb

static const struct { const char *n; const char *lv; const char *why; } DANGER[] = {
    { "gets",     "high",   "无长度限制" },
    { "strcpy",   "high",   "目标缓冲区无界" },
    { "strcat",   "high",   "目标缓冲区无界" },
    { "sprintf",  "high",   "目标缓冲区无界" },
    { "vsprintf", "high",   "目标缓冲区无界" },
    { "scanf",    "high",   "%s 无宽度限制时无界写入" },
    { "system",   "medium", "命令注入面" },
    { "popen",    "medium", "命令注入面" },
    { "strtok",   "medium", "非可重入" },
};

typedef struct { int found[sizeof DANGER / sizeof DANGER[0]]; int canary, fortify; } dscan;

static int dscan_cb(void *ud, const char *name, uint64_t addr, uint64_t size,
                    uint8_t info, uint16_t shndx)
{
    (void)addr; (void)size; (void)info; (void)shndx;
    dscan *d = ud;
    if (strcmp(name, "__stack_chk_fail") == 0)
        d->canary = 1;
    if (name[0] == '_' && name[1] == '_') {
        size_t l = strlen(name);
        if (l > 4 && strcmp(name + l - 4, "_chk") == 0)
            d->fortify = 1;
        return 0;
    }
    for (size_t i = 0; i < sizeof DANGER / sizeof DANGER[0]; i++)
        if (strcmp(name, DANGER[i].n) == 0)
            d->found[i] = 1;
    return 0;
}

void harden_report(const void *elfbuf, size_t len, const char *path, FILE *out, int json)
{
    elf_file *ef = elf_open_buf(elfbuf, len);
    if (!ef) {
        fprintf(out, json ? "{\"error\":\"not elf\"}\n" : "非 ELF 文件, 无法检查\n");
        return;
    }
    int gnustack = 0, stack_x = 0, relro_seg = 0;
    elf_phdr ph;
    for (size_t i = 0; i < elf_phdr_count(ef); i++) {
        elf_phdr_get(ef, i, &ph);
        if (ph.type == PT_GNU_STACK_) { gnustack = 1; if (ph.flags & 1) stack_x = 1; }
        if (ph.type == PT_GNU_RELRO_) relro_seg = 1;
    }
    int bind_now = 0;
    {
        elf_dyn dyn[128];
        size_t nd;
        if (elf_dynamic(ef, dyn, 128, &nd) == 0)
            for (size_t i = 0; i < nd; i++) {
                if (dyn[i].tag == DT_BIND_NOW_ && dyn[i].val) bind_now = 1;
                if (dyn[i].tag == DT_FLAGS_ && (dyn[i].val & 0x8)) bind_now = 1;
                if (dyn[i].tag == DT_FLAGS_1_ && (dyn[i].val & 0x1)) bind_now = 1;
            }
    }
    dscan d;
    memset(&d, 0, sizeof d);
    elf_for_each_sym(ef, dscan_cb, &d);
    int pie = (elf_type(ef) == 3);
    const char *relro = relro_seg ? (bind_now ? "full" : "partial") : "none";

    if (json) {
        fprintf(out, "{\"file\":\"%s\",\"pie\":%s,\"nx\":%s,\"relro\":\"%s\","
                     "\"canary\":%s,\"fortify\":%s,\"findings\":[",
                path, pie ? "true" : "false", (gnustack && !stack_x) ? "true" : "false",
                relro, d.canary ? "true" : "false", d.fortify ? "true" : "false");
        int f = 0;
        if (!gnustack || stack_x)
            fprintf(out, "%s{\"level\":\"high\",\"item\":\"stack_exec\"}", f++ ? "," : "");
        if (!relro_seg)
            fprintf(out, "%s{\"level\":\"high\",\"item\":\"no_relro\"}", f++ ? "," : "");
        if (!pie)
            fprintf(out, "%s{\"level\":\"medium\",\"item\":\"no_pie\"}", f++ ? "," : "");
        if (!d.canary)
            fprintf(out, "%s{\"level\":\"high\",\"item\":\"no_canary\"}", f++ ? "," : "");
        for (size_t i = 0; i < sizeof DANGER / sizeof DANGER[0]; i++)
            if (d.found[i])
                fprintf(out, "%s{\"level\":\"%s\",\"item\":\"%s\",\"why\":\"%s\"}",
                        f++ ? "," : "", DANGER[i].lv, DANGER[i].n, DANGER[i].why);
        fputs("]}\n", out);
    } else {
        fprintf(out, "== 安全检查: %s ==\n", path);
        fprintf(out, "  PIE     %s\n", pie ? "是" : "否 [中]");
        fprintf(out, "  NX      %s\n", (gnustack && !stack_x) ? "是" : "否 [高]");
        fprintf(out, "  RELRO   %s\n", relro_seg ? (bind_now ? "Full" : "Partial [低]") : "无 [高]");
        fprintf(out, "  Canary  %s\n", d.canary ? "是" : "否 [高]");
        fprintf(out, "  FORTIFY %s\n", d.fortify ? "是" : "否 [低]");
        int n = 0;
        for (size_t i = 0; i < sizeof DANGER / sizeof DANGER[0]; i++) n += d.found[i];
        fprintf(out, "  危险符号 %d 个\n", n);
        for (size_t i = 0; i < sizeof DANGER / sizeof DANGER[0]; i++)
            if (d.found[i])
                fprintf(out, "    [%s] %-10s %s\n", DANGER[i].lv, DANGER[i].n, DANGER[i].why);
    }
    elf_close(ef);
}
