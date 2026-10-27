/* vuln.c —— 漏洞分析：ELF 静态安全检查（加固项 + 危险函数）
 * 设计见 docs/03-vuln.md。判定依据全部来自 ELF 结构本身。 */
#include "common.h"
#include "elf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ELF 常量（本地定义）*/
#define PT_INTERP      3
#define PT_GNU_STACK   0x6474e551
#define PT_GNU_RELRO   0x6474e552
#define ET_EXEC_       2
#define ET_DYN_        3

#define DT_NULL_       0
#define DT_NEEDED_     1
#define DT_STRTAB_     5
#define DT_STRSZ_      10
#define DT_BIND_NOW_   24
#define DT_RPATH_      15
#define DT_RUNPATH_    29
#define DT_FLAGS_      30
#define DT_FLAGS_1_    0x6ffffffb
#define DF_BIND_NOW_   0x8
#define DF_1_NOW_      0x1

#define PF_X_          1

typedef enum { L_OK = 0, L_INFO, L_LOW, L_MED, L_HIGH } level_t;

typedef struct {
    const char *name;
    level_t     level;
    const char *reason;
} danger_fn;

static const danger_fn g_danger[] = {
    { "gets",    L_HIGH, "无长度限制, 已从 C 标准移除" },
    { "strcpy",  L_HIGH, "目标缓冲区无界" },
    { "strcat",  L_HIGH, "目标缓冲区无界" },
    { "sprintf", L_HIGH, "目标缓冲区无界" },
    { "vsprintf",L_HIGH, "目标缓冲区无界" },
    { "scanf",   L_HIGH, "%s 无宽度限制时无界写入" },
    { "fscanf",  L_HIGH, "%s 无宽度限制时无界写入" },
    { "sscanf",  L_HIGH, "%s 无宽度限制时无界写入" },
    { "system",  L_MED,  "shell 命令注入面" },
    { "popen",   L_MED,  "shell 命令注入面" },
    { "strtok",  L_MED,  "非可重入, 静态状态" },
    { "strncpy", L_LOW,  "有界但可能不写 NUL 终止" },
    { "strncat", L_LOW,  "有界但长度参数语义易错" },
};

typedef struct {
    const char *level_cn;
    const char *advice;
} level_meta;

static const char *level_name(level_t l)
{
    switch (l) {
    case L_HIGH: return "高";
    case L_MED:  return "中";
    case L_LOW:  return "低";
    default:     return "-";
    }
}

/* ---------- 危险符号扫描（去重）---------- */
typedef struct {
    const elf_file *ef;
    int  found[sizeof g_danger / sizeof g_danger[0]];
    int  canary, fortify, fortify_n;
} scan_ud;

static int sym_cb(void *ud, const char *name, uint64_t addr, uint64_t size,
                  uint8_t info, uint16_t shndx)
{
    (void)addr; (void)size; (void)info; (void)shndx;
    scan_ud *u = ud;
    if (strcmp(name, "__stack_chk_fail") == 0 || strcmp(name, "__stack_chk_guard") == 0)
        u->canary = 1;
    /* FORTIFY 替换形如 __memcpy_chk / __printf_chk */
    if (name[0] == '_' && name[1] == '_') {
        size_t n = strlen(name);
        if (n > 4 && strcmp(name + n - 4, "_chk") == 0) {
            u->fortify = 1;
            u->fortify_n++;
        }
        return 0; /* 跳过 __ 前缀的内部符号对危险函数的误报 */
    }
    for (size_t i = 0; i < sizeof g_danger / sizeof g_danger[0]; i++)
        if (strcmp(name, g_danger[i].name) == 0)
            u->found[i] = 1;
    return 0;
}

static const char *machine_name(uint16_t m)
{
    switch (m) {
    case 3:   return "x86";
    case 40:  return "ARM";
    case 62:  return "x86-64";
    case 243: return "RISC-V";
    case 183: return "AArch64";
    default:  return "其他";
    }
}

static void vuln_usage(void)
{
    puts("用法: oskit vuln <文件> [--json] [--strict] [--verbose]");
    puts("");
    puts("检查: PIE / NX / RELRO / Stack Canary / FORTIFY / 危险函数 / RPATH");
    puts("  --json    输出 JSON");
    puts("  --strict  存在高危发现时退出码 2 (CI 门禁)");
    puts("  --verbose 附带打印 NEEDED 依赖与 RPATH 值");
    puts("退出码: 0 完成, 1 文件/解析错误, 2 strict 命中高危");
}

int vuln_main(int argc, char **argv)
{
    const char *path = NULL;
    int json = 0, strict = 0, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "--json")) json = 1;
        else if (arg_is(argv[i], "--strict")) strict = 1;
        else if (arg_is(argv[i], "--verbose")) verbose = 1;
        else if (arg_is(argv[i], "--help") || arg_is(argv[i], "-h")) { vuln_usage(); return 0; }
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "oskit vuln: 未知选项 %s\n", argv[i]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "oskit vuln: 多余参数 %s\n", argv[i]);
            return 2;
        }
    }
    if (!path) {
        vuln_usage();
        return 2;
    }

    void *buf;
    size_t len;
    if (xread_file(path, &buf, &len) != 0)
        die("无法读取 %s: %s", path, strerror(errno));
    elf_file *ef = elf_open_buf(buf, len);
    if (!ef)
        die("%s 不是可解析的 ELF 文件(仅支持小端 ELF32/ELF64)", path);

    /* ---- PIE / NX / RELRO 判定 ---- */
    int has_interp = 0, has_gnustack = 0, stack_x = 0, has_relro_seg = 0;
    elf_phdr ph;
    for (size_t i = 0; i < elf_phdr_count(ef); i++) {
        elf_phdr_get(ef, i, &ph);
        if (ph.type == PT_INTERP)    has_interp = 1;
        if (ph.type == PT_GNU_STACK) { has_gnustack = 1; if (ph.flags & PF_X_) stack_x = 1; }
        if (ph.type == PT_GNU_RELRO) has_relro_seg = 1;
    }
    int pie; /* 0=否 1=是 2=共享库形态(天然位置无关) */
    if (elf_type(ef) == ET_EXEC_)      pie = 0;
    else if (elf_type(ef) == ET_DYN_)  pie = has_interp ? 1 : 2;
    else                               pie = 0;

    int bind_now = 0;
    int has_rpath = 0, has_runpath = 0;
    uint64_t rpath_str = 0, runpath_str = 0;
    char needed[64][64];
    size_t nneeded = 0;
    {
        elf_dyn dyn[256];
        size_t nd;
        uint64_t strtab = 0;
        if (elf_dynamic(ef, dyn, 256, &nd) == 0) {
            for (size_t i = 0; i < nd; i++) {
                if (dyn[i].tag == DT_BIND_NOW_ && dyn[i].val != 0) bind_now = 1;
                if (dyn[i].tag == DT_FLAGS_ && (dyn[i].val & DF_BIND_NOW_)) bind_now = 1;
                if (dyn[i].tag == DT_FLAGS_1_ && (dyn[i].val & DF_1_NOW_)) bind_now = 1;
                if (dyn[i].tag == DT_STRTAB_) strtab = dyn[i].val;
            }
            for (size_t i = 0; i < nd; i++) {
                if (dyn[i].tag == DT_NEEDED_ && nneeded < 64)
                    elf_read_cstr(ef, strtab + dyn[i].val, needed[nneeded++], 64);
                if (dyn[i].tag == DT_RPATH_)  { has_rpath = 1; rpath_str = strtab + dyn[i].val; }
                if (dyn[i].tag == DT_RUNPATH_){ has_runpath = 1; runpath_str = strtab + dyn[i].val; }
            }
        }
    }
    const char *relro = has_relro_seg ? (bind_now ? "Full" : "Partial") : "无";

    /* ---- 符号扫描 ---- */
    scan_ud ud = { ef, { 0 }, 0, 0, 0 };
    elf_for_each_sym(ef, sym_cb, &ud);

    /* ---- 汇总风险 ---- */
    int n_high = 0, n_med = 0, n_low = 0;
    if (!has_gnustack || stack_x) n_high++;         /* 栈可执行 */
    if (!has_relro_seg)         n_high++;           /* 无 RELRO */
    if (pie == 0)               n_med++;            /* 固定加载地址 */
    if (has_relro_seg && !bind_now) n_low++;        /* 仅 Partial RELRO */
    if (!ud.canary)             n_high++;           /* 无栈保护 */
    if (!ud.fortify)            n_low++;            /* 无 FORTIFY */
    if (has_rpath || has_runpath) n_low++;
    for (size_t i = 0; i < sizeof g_danger / sizeof g_danger[0]; i++) {
        if (!ud.found[i]) continue;
        if (g_danger[i].level == L_HIGH) n_high++;
        else if (g_danger[i].level == L_MED) n_med++;
        else n_low++;
    }

    if (json) {
        printf("{\"file\":");
        json_escape(stdout, path);
        printf(",\"arch\":\"%s\",\"bits\":%d,\"pie\":%d,\"nx\":%s,\"relro\":\"%s\","
               "\"canary\":%s,\"fortify\":%s,\"stripped\":%s,\"high\":%d,\"medium\":%d,\"low\":%d,"
               "\"findings\":[",
               machine_name(elf_machine(ef)), elf_is64(ef) ? 64 : 32,
               pie, (has_gnustack && !stack_x) ? "true" : "false", relro,
               ud.canary ? "true" : "false", ud.fortify ? "true" : "false",
               elf_has_symtab(ef) ? "false" : "true",
               n_high, n_med, n_low);
        int f = 0;
        if (!has_gnustack || stack_x)
            printf("%s{\"level\":\"high\",\"symbol\":\"stack_exec\",\"desc\":\"%s\"}",
                   f++ ? "," : "", stack_x ? "GNU_STACK 可执行" : "缺少 PT_GNU_STACK");
        if (!has_relro_seg)
            printf("%s{\"level\":\"high\",\"symbol\":\"no_relro\",\"desc\":\"缺少 PT_GNU_RELRO\"}",
                   f++ ? "," : "");
        if (pie == 0)
            printf("%s{\"level\":\"medium\",\"symbol\":\"no_pie\",\"desc\":\"ET_EXEC 固定地址\"}",
                   f++ ? "," : "");
        if (!ud.canary)
            printf("%s{\"level\":\"high\",\"symbol\":\"no_canary\",\"desc\":\"未启用栈保护\"}",
                   f++ ? "," : "");
        if (!ud.fortify)
            printf("%s{\"level\":\"low\",\"symbol\":\"no_fortify\",\"desc\":\"未启用 _FORTIFY_SOURCE\"}",
                   f++ ? "," : "");
        for (size_t i = 0; i < sizeof g_danger / sizeof g_danger[0]; i++) {
            if (!ud.found[i]) continue;
            printf("%s{\"level\":\"%s\",\"symbol\":\"%s\",\"desc\":",
                   f++ ? "," : "",
                   g_danger[i].level == L_HIGH ? "high" :
                   g_danger[i].level == L_MED  ? "medium" : "low",
                   g_danger[i].name);
            json_escape(stdout, g_danger[i].reason);
            putchar('}');
        }
        if (has_rpath || has_runpath)
            printf("%s{\"level\":\"low\",\"symbol\":\"%s\",\"desc\":\"运行期库搜索路径可被利用\"}",
                   f++ ? "," : "", has_rpath ? "rpath" : "runpath");
        puts("]}");
    } else {
        printf("== OSKit 漏洞分析: %s ==\n", path);
        printf("架构      %s · %d 位, %s, %s\n", machine_name(elf_machine(ef)),
               elf_is64(ef) ? 64 : 32,
               nneeded ? "动态链接" : "静态/无 NEEDED",
               elf_has_symtab(ef) ? "含符号表" : "已 strip(分析受限)");
        printf("PIE       %s%s\n", pie == 1 ? "是" : (pie == 2 ? "共享库形态" : "否"),
               pie == 0 ? "        [中] 建议 -fPIE -pie" : "");
        printf("NX        %s%s\n", (has_gnustack && !stack_x) ? "是" : "否",
               (has_gnustack && !stack_x) ? "" : "        [高] 栈可执行, 建议 -z noexecstack");
        printf("RELRO     %-8s %s\n", relro,
               !has_relro_seg ? "[高] 建议 -Wl,-z,relro,-z,now" :
               bind_now ? "" : "[低] 建议 -Wl,-z,now 升级为 Full");
        printf("Canary    %s%s\n", ud.canary ? "是" : "否",
               ud.canary ? "" : "        [高] 建议 -fstack-protector-strong");
        printf("FORTIFY   %s%s\n", ud.fortify ? "是" : "否",
               ud.fortify ? "" : "        [低] 建议 -O2 -D_FORTIFY_SOURCE=2");
        if (has_rpath || has_runpath) {
            char v[128];
            if (has_rpath && rpath_str && elf_read_cstr(ef, rpath_str, v, sizeof v) == 0)
                printf("RPATH     %s [低]\n", v);
            if (has_runpath && runpath_str && elf_read_cstr(ef, runpath_str, v, sizeof v) == 0)
                printf("RUNPATH   %s [低]\n", v);
        }
        int nd = 0;
        for (size_t i = 0; i < sizeof g_danger / sizeof g_danger[0]; i++)
            nd += ud.found[i];
        printf("危险函数  %d 个\n", nd);
        for (size_t i = 0; i < sizeof g_danger / sizeof g_danger[0]; i++) {
            if (!ud.found[i]) continue;
            printf("  [%s] %-10s %s\n", level_name(g_danger[i].level),
                   g_danger[i].name, g_danger[i].reason);
        }
        if (verbose && nneeded) {
            printf("依赖 (%zu):", nneeded);
            for (size_t i = 0; i < nneeded; i++)
                printf(" %s", needed[i]);
            putchar('\n');
        }
        printf("结论      高危 %d · 中危 %d · 低危 %d%s\n", n_high, n_med, n_low,
               (n_high && strict) ? "  → strict 模式: 不通过" : "");
    }

    elf_close(ef);
    free(buf);
    if (strict && n_high)
        return 2;
    return 0;
}
