/* cmd.c —— 命令中枢实现 */
#include "cmd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analyze.h"
#include "buildtool.h"
#include "elf.h"
#include "gdbstub.h"
#include "skeleton.h"

/* ---------- 内置演示程序（与 ../oskit/src/emu.c 同源, 编码已实测）---------- */
static const uint32_t DEMO1[] = {
    0x100002B7, 0x04500313, 0x00628023, 0x04D00313, 0x00628023, 0x05500313,
    0x00628023, 0x02000313, 0x00628023, 0x04F00313, 0x00628023, 0x04B00313,
    0x00628023, 0x00A00313, 0x00628023, 0x900003B7, 0x02A00E13, 0x01C3A023,
};
static const uint32_t DEMO2[] = {
    0x80800137, 0x100002B7, 0x06600F13, 0x01E28023, 0x06900F13, 0x01E28023,
    0x06200F13, 0x01E28023, 0x02800F13, 0x01E28023, 0x03100F13, 0x01E28023,
    0x03000F13, 0x01E28023, 0x02900F13, 0x01E28023, 0x03D00F13, 0x01E28023,
    0x00000513, 0x00100593, 0x00A00313, 0x00B503B3, 0x00058513, 0x00038593,
    0xFFF30313, 0xFE0318E3, 0x00050613, 0x00000313, 0x00A00E93, 0x03D67F33,
    0x03D65633, 0xFFC10113, 0x01E12023, 0x00130313, 0xFE0614E3, 0x00012F03,
    0x00410113, 0x030F0F13, 0x01E28023, 0xFFF30313, 0xFE0316E3, 0x00A00F13,
    0x01E28023, 0x90000FB7, 0x00AFA023,
};

/* ---------- 控制台 ---------- */
static void con_commit(app_t *a, const char *line)
{
    snprintf(a->con[a->con_head], CON_COLS, "%s", line);
    a->con_head = (a->con_head + 1) % CON_LINES;
    if (a->con_count < CON_LINES)
        a->con_count++;
    if (a->con_scroll > 0)
        a->con_scroll = 0;                 /* 新输出时回到贴底 */
    if (a->emit)
        a->emit(a->emit_ud, line);
}

void app_con(app_t *a, const char *fmt, ...)
{
    char tmp[CON_COLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    size_t l = strlen(tmp);
    while (l && tmp[l - 1] == '\n')
        tmp[--l] = 0;                      /* 去尾换行, 环形按行存 */
    con_commit(a, tmp);
}

void app_con_block(app_t *a, const char *text)
{
    char line[CON_COLS];
    size_t n = 0;
    for (const char *p = text; ; p++) {
        if (*p && *p != '\n' && n + 1 < sizeof line)
            line[n++] = *p;
        else {
            line[n] = 0;
            if (n || *p == '\n')
                con_commit(a, line);
            n = 0;
        }
        if (!*p)
            break;
    }
}

void app_uart_cb(void *ud, uint8_t b)
{
    app_t *a = ud;
    if (b == '\n' || a->uart_n >= CON_COLS - 1) {
        a->uart_cur[a->uart_n] = 0;
        if (a->uart_n)
            con_commit(a, a->uart_cur);
        a->uart_n = 0;
        if (b != '\n') {
            a->uart_cur[a->uart_n++] = (char)b;
        }
    } else {
        a->uart_cur[a->uart_n++] = (char)b;
    }
}

void app_trace_cb(void *ud, uint32_t pc, uint32_t inst)
{
    app_t *a = ud;
    if (!a->trace_on)
        return;
    char d[96];
    rv_disasm(inst, d, sizeof d);
    app_con(a, "%08x: %08x  %s", pc, inst, d);
}

/* ---------- 装载 ---------- */
static int read_file(const char *path, void **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return -1; }
    char *p = malloc(sz ? (size_t)sz : 1);
    if (!p) { fclose(f); return -1; }
    if (sz && fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p); fclose(f); return -1;
    }
    fclose(f);
    *buf = p;
    *len = (size_t)sz;
    return 0;
}

void app_unload(app_t *a)
{
    free(a->elfbuf);
    a->elfbuf = NULL;
    a->elflen = 0;
    a->ef = NULL;                          /* ef 指向 elfbuf 上的视图, 随之失效 */
    free(a->syms);
    a->syms = NULL;
    a->nsyms = 0;
    a->progname[0] = 0;
    if (a->vm)
        rv_free(a->vm);
    a->vm = NULL;
}

static void vm_hooks(app_t *a)
{
    a->vm->uart_fn = app_uart_cb;
    a->vm->uart_ud = a;
    a->vm->trace_fn = app_trace_cb;
    a->vm->trace_ud = a;
}

int app_open_prog(app_t *a, const char *path)
{
    void *buf;
    size_t len;
    if (read_file(path, &buf, &len) != 0) {
        app_con(a, "open: 无法读取 %s", path);
        return -1;
    }
    app_unload(a);
    a->elfbuf = buf;
    a->elflen = len;
    elf_file *ef = elf_open_buf(buf, len);
    if (!ef) {
        app_con(a, "open: %s 不是合法 ELF", path);
        app_unload(a);
        return -1;
    }
    a->ef = ef;
    snprintf(a->progname, sizeof a->progname, "%s", path);
    analyze_collect_syms(ef, &a->syms, &a->nsyms);

    /* 尝试载入 RV32 VM —— 失败(如 ELF64/x86 目标)不影响 harden 等纯 ELF 检查 */
    a->vm = rv_create(8);
    if (!a->vm) {
        app_con(a, "open: 内存不足(ELF 已识别, VM 未建)");
        return 0;
    }
    vm_hooks(a);
    if (rv_load_elf(a->vm, buf, len) != 0) {
        app_con(a, "open: ELF 已识别(符号 %zu), 但不可载入 RV32 VM: %s",
                a->nsyms, a->vm->err);
        rv_free(a->vm);
        a->vm = NULL;
        return 0;
    }
    a->entry = a->vm->pc;
    if (a->syms)
        rv_set_syms(a->vm, a->syms, a->nsyms);
    a->mem_addr = a->entry;
    app_con(a, "已装载 %s: 入口 0x%08x, 符号 %zu 个", path, a->entry, a->nsyms);
    return 0;
}

void app_load_demo(app_t *a, int which)
{
    app_unload(a);
    a->vm = rv_create(8);
    if (!a->vm) {
        app_con(a, "demo: 内存不足");
        return;
    }
    vm_hooks(a);
    const uint32_t *code = which == 2 ? DEMO2 : DEMO1;
    size_t words = which == 2 ? sizeof DEMO2 / 4 : sizeof DEMO1 / 4;
    rv_load_flat(a->vm, code, words * 4, RV_RAM_BASE);
    a->entry = RV_RAM_BASE;
    snprintf(a->progname, sizeof a->progname, "demo%d (内置)", which);
    a->mem_addr = RV_RAM_BASE;
    app_con(a, "已装载内置 demo%d (%zu 条指令, 入口 0x%08x)", which, words, a->entry);
}

/* ---------- 小工具 ---------- */
static int parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s)
        return -1;
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    if (*end)
        return -1;
    *out = v;
    return 0;
}

static int64_t find_sym(app_t *a, const char *name)
{
    for (size_t i = 0; i < a->nsyms; i++)
        if (strcmp(a->syms[i].name, name) == 0)
            return (int64_t)a->syms[i].addr;
    return -1;
}

static void print_regs(app_t *a)
{
    if (!a->vm) { app_con(a, "regs: 未装载程序"); return; }
    for (int r = 0; r < 32; r += 4)
        app_con(a, "%3s=%08x  %3s=%08x  %3s=%08x  %3s=%08x",
                rv_reg_name(r),     a->vm->x[r],
                rv_reg_name(r + 1), a->vm->x[r + 1],
                rv_reg_name(r + 2), a->vm->x[r + 2],
                rv_reg_name(r + 3), a->vm->x[r + 3]);
    app_con(a, " pc=%08x  instret=%llu", a->vm->pc,
            (unsigned long long)a->vm->instret);
}

static void print_mem(app_t *a)
{
    if (!a->vm) { app_con(a, "mem: 未装载程序"); return; }
    for (int row = 0; row < 4; row++) {
        uint32_t addr = a->mem_addr + (uint32_t)(row * 8);
        uint8_t b[8];
        if (rv_peek(a->vm, addr, b, 8) != 0) {
            app_con(a, "%08x: <未映射>", addr);
            continue;
        }
        char hex[24] = "";
        for (int k = 0; k < 8; k++)
            snprintf(hex + k * 3, 4, "%02x ", b[k]);
        char txt[12] = "";
        for (int k = 0; k < 8; k++)
            txt[k] = (b[k] >= 0x20 && b[k] < 0x7f) ? (char)b[k] : '.';
        app_con(a, "%08x: %s |%s|", addr, hex, txt);
    }
}

static void print_disas(app_t *a, int n)
{
    if (!a->vm) { app_con(a, "disas: 未装载程序"); return; }
    if (n > 32)
        n = 32;
    for (int i = 0; i < n; i++) {
        uint32_t addr = a->vm->pc + (uint32_t)(i * 4);
        uint8_t b[4];
        if (rv_peek(a->vm, addr, b, 4) != 0) {
            app_con(a, "%08x: <未映射>", addr);
            break;
        }
        uint32_t in;
        memcpy(&in, b, 4);
        char d[96];
        rv_disasm(in, d, sizeof d);
        app_con(a, "%08x: %08x  %s", addr, in, d);
    }
}

/* memstream 输出 → 控制台(多行) */
#define WITH_MEMSTREAM(a, call)                                     \
    do {                                                            \
        char *mb = NULL;                                            \
        size_t msz = 0;                                             \
        FILE *mf = open_memstream(&mb, &msz);                       \
        if (mf) {                                                   \
            call;                                                   \
            fclose(mf);                                             \
            if (mb) { app_con_block(a, mb); free(mb); }             \
        }                                                           \
    } while (0)

/* ---------- 命令表 ---------- */
void cmd_help(app_t *a)
{
    app_con_block(a,
        "命令: open <file.elf> | demo [1|2] | edit <file> | w(保存) | build [file]\n"
        "      skeleton <riscv32|riscv64> <name> [dir] | ls [dir]\n"
        "      run [max] | step [n] | reset | bp <sym|0xaddr> | bpdel | regs\n"
        "      mem <0xaddr> | disas [n] | trace on|off\n"
        "      analyze [--json] | harden [--json] | stats [--json]\n"
        "      gdb <port>(仅 headless) | quit");
}

int cmd_exec(app_t *a, char *line)
{
    /* 去注释(# 开头)与空白, 分词 */
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#')
        return 0;
    char *argv[8];
    int argc = 0;
    char *save = NULL;
    for (char *t = strtok_r(p, " \t\r\n", &save);
         t && argc < 8;
         t = strtok_r(NULL, " \t\r\n", &save))
        argv[argc++] = t;
    if (!argc)
        return 0;
    const char *cmd = argv[0];
    const char *a1 = argc > 1 ? argv[1] : NULL;
    const char *a2 = argc > 2 ? argv[2] : NULL;
    int json = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--json") == 0)
            json = 1;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help(a);
    } else if (strcmp(cmd, "open") == 0 || strcmp(cmd, "load") == 0) {
        if (!a1) { app_con(a, "用法: open <file.elf>"); return 1; }
        return app_open_prog(a, a1) == 0 ? 0 : 1;
    } else if (strcmp(cmd, "demo") == 0) {
        app_load_demo(a, a1 && atoi(a1) == 2 ? 2 : 1);
    } else if (strcmp(cmd, "edit") == 0) {
        if (!a1) { app_con(a, "用法: edit <file>"); return 1; }
        if (!a->tui_mode) { app_con(a, "edit 仅在 TUI 模式可用(headless 请用 $EDITOR)"); return 1; }
        buf_free(&a->ed);
        buf_open(&a->ed, a1);
        app_con(a, "编辑 %s (%zu 行)", a1, a->ed.n);
    } else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "write") == 0) {
        if (!a->ed.path[0]) { app_con(a, "没有打开的文件"); return 1; }
        if (buf_save(&a->ed) == 0)
            app_con(a, "已保存 %s", a->ed.path);
        else
            app_con(a, "保存失败 %s", a->ed.path);
    } else if (strcmp(cmd, "build") == 0) {
        const char *src = a1 ? a1 : (a->ed.path[0] ? a->ed.path : NULL);
        if (!src) { app_con(a, "用法: build [file.c|file.s] (或先 edit)"); return 1; }
        const char *cc = buildtool_find_cc();
        if (!cc) {
            app_con(a, "build: 未找到 RISC-V 交叉工具链");
            app_con(a, "  安装: sudo apt install gcc-riscv64-unknown-elf");
            app_con(a, "  或设: export RISCV_CC=<编译器路径>");
            return 1;
        }
        problem_t probs[32];
        int nprob = 0;
        char log[2048];
        int rc = buildtool_build(src, "rvlab-build.elf", probs, 32, &nprob, log, sizeof log);
        app_con(a, "build[%s]: 退出码 %d, 诊断 %d 条", cc, rc, nprob);
        for (int i = 0; i < nprob; i++)
            app_con(a, "  %s:%d: [%c] %s", probs[i].file, probs[i].line, probs[i].sev, probs[i].msg);
        if (rc == 0) {
            app_con(a, "产物 rvlab-build.elf, 自动装载...");
            return app_open_prog(a, "rvlab-build.elf") == 0 ? 0 : 1;
        }
        if (log[0])
            app_con_block(a, log);
        return 1;
    } else if (strcmp(cmd, "skeleton") == 0) {
        if (!a1 || !a2) { app_con(a, "用法: skeleton <riscv32|riscv64> <name> [dir]"); return 1; }
        const char *dir = argc > 3 && argv[3][0] != '-' ? argv[3] : ".";
        int rc = skeleton_gen(a1, a2, dir);
        if (rc == 0)
            app_con(a, "已生成 %s/%s (crt0.S linker.ld kernel.c Makefile run.sh README.md)", dir, a2);
        else if (rc == -2)
            app_con(a, "目录已存在");
        else
            app_con(a, "生成失败(%d)", rc);
        return rc == 0 ? 0 : 1;
    } else if (strcmp(cmd, "ls") == 0) {
        /* TUI 文件面板之外的 headless 列目录 */
        const char *dir = a1 ? a1 : ".";
        char cmd2[300];
        snprintf(cmd2, sizeof cmd2, "ls -p --color=never '%s' 2>/dev/null | head -40", dir);
        FILE *f = popen(cmd2, "r");
        if (!f) return 1;
        char ln[256];
        int n = 0;
        while (fgets(ln, sizeof ln, f)) {
            ln[strcspn(ln, "\r\n")] = 0;
            app_con(a, "  %s", ln);
            n++;
        }
        pclose(f);
        if (!n)
            app_con(a, "(空或不可读: %s)", dir);
    } else if (strcmp(cmd, "run") == 0) {
        if (!a->vm) { app_con(a, "run: 未装载程序 (open/demo)"); return 1; }
        uint64_t max = a->run_max ? a->run_max : 50000000ull;
        if (a1 && parse_u64(a1, &max) != 0) {
            app_con(a, "run: 参数应为数字");
            return 1;
        }
        int r = rv_run(a->vm, max);
        if (r == RV_EXIT)
            app_con(a, "程序退出, 代码 %d (执行 %llu 条)", a->vm->exit_code,
                    (unsigned long long)a->vm->instret);
        else if (r == RV_BREAK)
            app_con(a, "命中断点 0x%08llx (未执行)", (unsigned long long)a->vm->hit_bp);
        else if (r == RV_MAXSTEPS)
            app_con(a, "达到步数上限 %llu, 停在 0x%08x", (unsigned long long)max, a->vm->pc);
        else {
            app_con(a, "执行错误: %s", a->vm->err);
            return 1;
        }
    } else if (strcmp(cmd, "step") == 0 || strcmp(cmd, "s") == 0) {
        if (!a->vm) { app_con(a, "step: 未装载程序"); return 1; }
        long n = a1 ? atol(a1) : 1;
        if (n < 1) n = 1;
        if (n > 10000) n = 10000;
        for (long i = 0; i < n; i++) {
            int r = rv_step(a->vm);
            if (r == RV_BREAK) {
                app_con(a, "命中断点 0x%08llx (未执行)", (unsigned long long)a->vm->hit_bp);
                break;
            }
            if (r == RV_EXIT) {
                app_con(a, "程序退出, 代码 %d", a->vm->exit_code);
                break;
            }
            if (r == RV_ERR) {
                app_con(a, "执行错误: %s", a->vm->err);
                return 1;
            }
        }
        if (a->vm && !a->vm->exited) {
            uint8_t b[4];
            uint32_t in = 0;
            if (rv_peek(a->vm, a->vm->pc, b, 4) == 0)
                memcpy(&in, b, 4);
            char d[96];
            rv_disasm(in, d, sizeof d);
            app_con(a, "pc=%08x  下一句: %s", a->vm->pc, d);
        }
    } else if (strcmp(cmd, "reset") == 0) {
        if (!a->vm) { app_con(a, "reset: 未装载程序"); return 1; }
        rv_reset(a->vm, a->entry);
        a->uart_n = 0;
        app_con(a, "已重置, pc=0x%08x", a->vm->pc);
    } else if (strcmp(cmd, "bp") == 0 || strcmp(cmd, "break") == 0) {
        if (!a->vm) { app_con(a, "bp: 未装载程序"); return 1; }
        if (!a1) {
            for (int i = 0; i < a->vm->n_bp; i++)
                app_con(a, "  [%d] 0x%08llx", i, (unsigned long long)a->vm->bp[i]);
            if (!a->vm->n_bp)
                app_con(a, "(无断点)");
            return 0;
        }
        uint64_t addr;
        if (parse_u64(a1, &addr) != 0) {
            int64_t s = find_sym(a, a1);
            if (s < 0) { app_con(a, "bp: 未知符号 %s", a1); return 1; }
            addr = (uint64_t)s;
        }
        if (rv_bp_add(a->vm, addr) != 0) {
            app_con(a, "bp: 断点已满");
            return 1;
        }
        app_con(a, "断点 [%d] @ 0x%08llx", a->vm->n_bp - 1, (unsigned long long)addr);
    } else if (strcmp(cmd, "bpdel") == 0) {
        if (!a->vm) return 1;
        rv_bp_clear(a->vm);
        app_con(a, "已清空断点");
    } else if (strcmp(cmd, "regs") == 0) {
        print_regs(a);
    } else if (strcmp(cmd, "mem") == 0) {
        if (!a1) { print_mem(a); return 0; }
        uint64_t addr;
        if (parse_u64(a1, &addr) != 0) { app_con(a, "mem: 地址格式 0x..."); return 1; }
        a->mem_addr = (uint32_t)addr;
        print_mem(a);
    } else if (strcmp(cmd, "disas") == 0) {
        print_disas(a, a1 ? atoi(a1) : 8);
    } else if (strcmp(cmd, "trace") == 0) {
        a->trace_on = !a1 ? !a->trace_on : (strcmp(a1, "on") == 0 || strcmp(a1, "1") == 0);
        app_con(a, "指令跟踪 %s", a->trace_on ? "开" : "关");
    } else if (strcmp(cmd, "analyze") == 0) {
        if (!a->vm) { app_con(a, "analyze: 未装载程序"); return 1; }
        WITH_MEMSTREAM(a, analyze_report(a->vm, mf, json));
    } else if (strcmp(cmd, "harden") == 0) {
        if (!a->ef) { app_con(a, "harden: 未装载 ELF(demo 无文件)"); return 1; }
        WITH_MEMSTREAM(a, harden_report(a->elfbuf, a->elflen, a->progname, mf, json));
    } else if (strcmp(cmd, "stats") == 0) {
        if (!a->vm) { app_con(a, "stats: 未装载程序"); return 1; }
        rv_vm *v = a->vm;
        if (json) {
            app_con(a, "{\"instructions\":%llu,\"branches\":%llu,\"taken\":%llu,"
                       "\"loads\":%llu,\"stores\":%llu,\"exit\":%d}",
                    (unsigned long long)v->instret, (unsigned long long)v->br_total,
                    (unsigned long long)v->br_taken, (unsigned long long)v->load_cnt,
                    (unsigned long long)v->store_cnt, v->exit_code);
        } else {
            app_con(a, "指令 %llu · 分支 %llu (命中 %.1f%%) · load %llu · store %llu · %s",
                    (unsigned long long)v->instret, (unsigned long long)v->br_total,
                    v->br_total ? 100.0 * v->br_taken / v->br_total : 0.0,
                    (unsigned long long)v->load_cnt, (unsigned long long)v->store_cnt,
                    v->exited ? "已退出" : "未退出");
        }
    } else if (strcmp(cmd, "gdb") == 0) {
        if (a->tui_mode) {
            app_con(a, "gdb: TUI 模式下请用 'rvlab --gdbstub <port>' 启动");
            return 1;
        }
        int port = a1 ? atoi(a1) : 1234;
        if (port <= 0 || port > 65535) { app_con(a, "gdb: 端口非法"); return 1; }
        return gdbstub_serve(a, port);
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
        a->quit = 1;
    } else {
        app_con(a, "未知命令: %s (help 查看)", cmd);
        return 1;
    }
    return 0;
}
