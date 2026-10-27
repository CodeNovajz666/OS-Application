/* debug.c —— 软件调试器：ptrace + INT3 软件断点 + DRn 硬件观察点（仅 x86-64）
 * 设计见 docs/04-debug.md。 */
#if !defined(__x86_64__)
#error "oskit debug 仅支持 x86-64 宿主(寄存器布局与断点机制为 x86 语义)"
#endif

#include "common.h"
#include "elf.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_BP   64
#define MAX_SYM  8192

/* ---------- 符号缓存 ---------- */
typedef struct {
    char     name[96];
    uint64_t addr, size;   /* 文件内偏移(未加基址) */
} sym_t;

static sym_t  g_syms[MAX_SYM];
static size_t g_nsym;
static elf_file *g_elf;
static uint64_t  g_base;     /* PIE 加载基址(ET_EXEC 时为 0) */
static int       g_pie;

/* ---------- 断点 / 观察点 ---------- */
typedef struct {
    int      used, num;
    uint64_t addr;           /* 运行时地址 */
    long     orig_word;      /* 改写前的 8 字节原值 */
    char     label[80];
} bp_t;

static bp_t     g_bps[MAX_BP];
static uint64_t g_bp_step_addr;    /* 非 0: 正站在该断点上, 原指令已恢复待单步 */
static int      g_ss_pending;      /* 正在等待 SINGLESTEP 的陷入(区别于断点陷入) */
static uint64_t g_watch[4];        /* 硬件观察点地址, 0=空闲 */
/* 程序未启动时登记的断点(run 后计算 PIE 基址再落实) */
static char g_pending_bp[32][96];
static int  g_n_pend;

/* ---------- 目标进程 ---------- */
static pid_t g_child;
static int   g_dead = 1;
static int   g_last_sig;
static struct user_regs_struct g_regs;
static char *g_prog;
static char **g_prog_args;         /* exec 用参数向量(含 argv[0]) */
static char  g_prog_args_buf[4096];
static char *g_argv_slots[64];

/* ===================================================================== */
/* ptrace 原语                                                            */
/* ===================================================================== */
static long peek_data(uint64_t addr)
{
    errno = 0;
    long v = ptrace(PTRACE_PEEKDATA, g_child, (void *)addr, 0);
    if (v == -1 && errno)
        return -1;
    return v;
}

static int poke_data(uint64_t addr, long word)
{
    return ptrace(PTRACE_POKEDATA, g_child, (void *)addr, (void *)word) == -1 ? -1 : 0;
}

static uint64_t peek_user_off(size_t off)
{
    errno = 0;
    long v = ptrace(PTRACE_PEEKUSER, g_child, (void *)off, 0);
    if (v == -1 && errno)
        return (uint64_t)-1;
    return (uint64_t)v;
}

static int poke_user_off(size_t off, uint64_t val)
{
    return ptrace(PTRACE_POKEUSER, g_child, (void *)off, (void *)(uintptr_t)val) == -1 ? -1 : 0;
}

static int get_regs(void)
{
    return ptrace(PTRACE_GETREGS, g_child, 0, &g_regs) == -1 ? -1 : 0;
}

static int set_regs(void)
{
    return ptrace(PTRACE_SETREGS, g_child, 0, &g_regs) == -1 ? -1 : 0;
}

/* ===================================================================== */
/* 符号                                                                   */
/* ===================================================================== */
static int sym_collect(void *ud, const char *name, uint64_t addr, uint64_t size,
                       uint8_t info, uint16_t shndx)
{
    (void)ud;
    uint8_t type = ELF_SYM_TYPE(info);
    if (shndx == 0 || (type != 2 && type != 1 && type != 10)) /* FUNC/OBJECT/IFUNC */
        return 0;
    if (g_nsym >= MAX_SYM)
        return 1;
    snprintf(g_syms[g_nsym].name, sizeof g_syms[0].name, "%s", name);
    g_syms[g_nsym].addr = addr;
    g_syms[g_nsym].size = size;
    g_nsym++;
    return 0;
}

static const char *sym_name_at(uint64_t runtime_addr) /* 运行时地址 → 符号 */
{
    uint64_t off = runtime_addr - g_base;
    for (size_t i = 0; i < g_nsym; i++) {
        if (off == g_syms[i].addr)
            return g_syms[i].name;
        if (g_syms[i].size && off >= g_syms[i].addr && off < g_syms[i].addr + g_syms[i].size)
            return g_syms[i].name;
    }
    return NULL;
}

/* 解析 "main" | "0x401100" | "+0x20"(相对加载基址) | "$rdi"(寄存器) */
static int parse_addr(const char *s, uint64_t *out)
{
    if (!s || !*s)
        return -1;
    if (s[0] == '$') {
        const char *r = s + 1;
#define R(nm) (strcmp(r, #nm) == 0) { *out = g_regs.nm; return 0; }
        if R(rax) else if R(rbx) else if R(rcx) else if R(rdx)
        else if R(rsi) else if R(rdi) else if R(rbp) else if R(rsp)
        else if R(r8) else if R(r9) else if R(r10) else if R(r11)
        else if R(r12) else if R(r13) else if R(r14) else if R(r15)
        else if R(rip)
#undef R
            return -1;
    }
    if (s[0] == '+') {
        char *end;
        unsigned long long v = strtoull(s + 1, &end, 0);
        if (*end) return -1;
        *out = g_base + v;
        return 0;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char *end;
        unsigned long long v = strtoull(s, &end, 0);
        if (*end) return -1;
        *out = v;
        return 0;
    }
    for (size_t i = 0; i < g_nsym; i++)
        if (strcmp(g_syms[i].name, s) == 0) {
            *out = g_base + g_syms[i].addr;
            return 0;
        }
    return -1;
}

/* ===================================================================== */
/* 断点                                                                   */
/* ===================================================================== */
static int bp_insert(uint64_t addr, const char *label)
{
    int slot = -1;
    for (int i = 0; i < MAX_BP; i++)
        if (!g_bps[i].used) { slot = i; break; }
    if (slot < 0) {
        printf("断点已满(%d)\n", MAX_BP);
        return -1;
    }
    long w = peek_data(addr);
    if (w == -1 && errno) {
        printf("无法读取地址 0x%llx (先 run 启动程序?)\n", (unsigned long long)addr);
        return -1;
    }
    if (poke_data(addr, (w & ~0xffL) | 0xcc) != 0) {
        printf("写入断点失败: %s\n", strerror(errno));
        return -1;
    }
    int num = 1;
    for (int i = 0; i < MAX_BP; i++)
        if (g_bps[i].used && g_bps[i].num >= num) num = g_bps[i].num + 1;
    g_bps[slot].used = 1;
    g_bps[slot].num = num;
    g_bps[slot].addr = addr;
    g_bps[slot].orig_word = w;
    snprintf(g_bps[slot].label, sizeof g_bps[0].label, "%s", label);
    printf("[%d] %s @ 0x%llx\n", num, label, (unsigned long long)addr);
    return 0;
}

static void bp_restore(uint64_t addr) /* 恢复原字节(不动 rip) */
{
    for (int i = 0; i < MAX_BP; i++)
        if (g_bps[i].used && g_bps[i].addr == addr) {
            poke_data(addr, g_bps[i].orig_word);
            return;
        }
}

static void bp_reinsert(uint64_t addr) /* 补回 INT3 */
{
    for (int i = 0; i < MAX_BP; i++)
        if (g_bps[i].used && g_bps[i].addr == addr) {
            long w = peek_data(addr);
            if (w != -1 || !errno)
                poke_data(addr, (w & ~0xffL) | 0xcc);
            return;
        }
}

static int bp_at(uint64_t addr)
{
    for (int i = 0; i < MAX_BP; i++)
        if (g_bps[i].used && g_bps[i].addr == addr)
            return 1;
    return 0;
}

/* 按 "main" | "*0x401100" | "*+0x20" 规格设断点(运行时地址需已可解析) */
static void try_break(const char *spec)
{
    uint64_t addr;
    if (spec[0] == '*') {
        if (parse_addr(spec + 1, &addr) != 0 && parse_addr(spec, &addr) != 0) {
            printf("无法解析: %s\n", spec);
            return;
        }
        bp_insert(addr, spec);
    } else if (parse_addr(spec, &addr) == 0) {
        bp_insert(addr, spec);
    } else {
        printf("未知符号: %s\n", spec);
    }
}

/* ===================================================================== */
/* 进程启动 / 事件等待                                                    */
/* ===================================================================== */
static uint64_t find_load_base(pid_t pid)
{
    /* 取 maps 中该可执行文件第一条映射的起始地址(PIE 基址) */
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    const char *base = strrchr(g_prog, '/');
    base = base ? base + 1 : g_prog;
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, base)) {
            fclose(f);
            return strtoull(line, NULL, 16);
        }
    }
    fclose(f);
    return 0;
}

static void *g_prog_buf; /* 被调试程序文件内容(launch 间复用/释放) */

static void launch(void)
{
    /* 清理旧断点的 INT3(进程已死, 数据不可写, 只清登记) */
    g_bp_step_addr = 0;
    g_nsym = 0;
    if (g_elf) { elf_close(g_elf); g_elf = NULL; }
    free(g_prog_buf);
    g_prog_buf = NULL;

    void *buf = NULL;
    size_t len = 0;
    g_elf = (xread_file(g_prog, &buf, &len) == 0) ? elf_open_buf(buf, len) : NULL;
    g_prog_buf = buf;
    if (!g_elf)
        printf("警告: %s 无符号表可解析(符号名断点不可用, 地址断点可用)\n", g_prog);
    else {
        elf_for_each_sym(g_elf, sym_collect, NULL);
        g_pie = (elf_type(g_elf) == 3);
    }

    pid_t p = fork();
    if (p < 0) {
        printf("fork 失败: %s\n", strerror(errno));
        return;
    }
    if (p == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execvp(g_prog, g_prog_args);
        fprintf(stderr, "exec %s 失败: %s\n", g_prog, strerror(errno));
        _exit(127);
    }
    g_child = p;
    int st;
    if (waitpid(p, &st, 0) != p || !WIFSTOPPED(st)) {
        printf("子进程启动异常\n");
        g_dead = 1;
        return;
    }
    g_dead = 0;
    g_last_sig = 0;
    g_base = g_pie ? find_load_base(p) : 0;
    get_regs();
    printf("程序已启动, pid=%d, %s, 加载基址 0x%llx, 入口 rip=0x%llx\n",
           p, g_pie ? "PIE" : "非PIE",
           (unsigned long long)g_base, (unsigned long long)g_regs.rip);
    /* 落实 run 之前登记的断点 */
    for (int i = 0; i < g_n_pend; i++)
        try_break(g_pending_bp[i]);
    g_n_pend = 0;
}

/* 0=停下 1=结束 -1=错误 */
static int wait_stop(void)
{
    int st;
    if (waitpid(g_child, &st, 0) != g_child)
        return -1;
    if (WIFEXITED(st)) {
        printf("程序已退出, 代码 %d\n", WEXITSTATUS(st));
        g_dead = 1;
        return 1;
    }
    if (WIFSIGNALED(st)) {
        printf("程序被信号 %d 终止\n", WTERMSIG(st));
        g_dead = 1;
        return 1;
    }
    int sig = WSTOPSIG(st);
    if (get_regs() != 0)
        return -1;

    if (sig == SIGTRAP) {
        /* 硬件观察点? */
        uint64_t dr6 = peek_user_off(offsetof(struct user, u_debugreg[6]));
        if (dr6 != (uint64_t)-1 && (dr6 & 0xf)) {
            for (int i = 0; i < 4; i++)
                if (dr6 & (1u << i))
                    printf("观察点 #%d 触发 @ 0x%llx, 停在 rip=0x%llx\n",
                           i, (unsigned long long)g_watch[i],
                           (unsigned long long)g_regs.rip);
            poke_user_off(offsetof(struct user, u_debugreg[6]), 0); /* 清 DR6 */
            g_ss_pending = 0;
            return 0;
        }
        /* 单步结果: 首条指令可能是 1 字节(如 push rbp), rip-1 恰等于断点地址,
         * 必须先于断点判定处理, 否则会被误判为"再次命中"而死循环 */
        if (g_ss_pending) {
            g_ss_pending = 0;
            return 0;
        }
        /* 软件断点? */
        uint64_t addr = g_regs.rip - 1;
        if (bp_at(addr)) {
            bp_restore(addr);
            g_regs.rip = addr;
            set_regs();
            g_bp_step_addr = addr;
            for (int i = 0; i < MAX_BP; i++)
                if (g_bps[i].used && g_bps[i].addr == addr)
                    printf("命中断点 #%d %s @ 0x%llx\n", g_bps[i].num,
                           g_bps[i].label, (unsigned long long)addr);
            return 0;
        }
        printf("SIGTRAP 停在 rip=0x%llx\n", (unsigned long long)g_regs.rip);
        return 0;
    }

    printf("收到信号 %d, 停在 rip=0x%llx (continue 将吞掉, signal <n> 可投递)\n",
           sig, (unsigned long long)g_regs.rip);
    g_last_sig = sig;
    return 0;
}

static int step_one(int deliver)
{
    g_ss_pending = 1;
    if (ptrace(PTRACE_SINGLESTEP, g_child, 0, (void *)(intptr_t)deliver) == -1)
        return -1;
    int r = wait_stop();
    if (r == 0 && g_bp_step_addr) {   /* 越过了断点指令, 补回 INT3 */
        bp_reinsert(g_bp_step_addr);
        g_bp_step_addr = 0;
    }
    return r;
}

static int cont_run(int deliver)
{
    if (g_bp_step_addr) {             /* 先单步越过当前断点 */
        int r = step_one(0);
        if (r != 0)
            return r;
    }
    if (ptrace(PTRACE_CONT, g_child, 0, (void *)(intptr_t)deliver) == -1)
        return -1;
    return wait_stop();
}

/* ===================================================================== */
/* 命令                                                                   */
/* ===================================================================== */
static void print_regs(void)
{
    const struct { const char *n; unsigned long long v; } r[] = {
        { "rax", g_regs.rax }, { "rbx", g_regs.rbx }, { "rcx", g_regs.rcx },
        { "rdx", g_regs.rdx }, { "rsi", g_regs.rsi }, { "rdi", g_regs.rdi },
        { "rbp", g_regs.rbp }, { "rsp", g_regs.rsp }, { " r8", g_regs.r8  },
        { " r9", g_regs.r9  }, { "r10", g_regs.r10 }, { "r11", g_regs.r11 },
        { "r12", g_regs.r12 }, { "r13", g_regs.r13 }, { "r14", g_regs.r14 },
        { "r15", g_regs.r15 },
    };
    for (int i = 0; i < 16; i++) {
        printf("%s=0x%016llx", r[i].n, r[i].v);
        putchar((i % 4 == 3) ? '\n' : ' ');
    }
    printf("rip=0x%016llx  rflags=0x%llx  (bit1 traps=%d direction=%d)\n",
           (unsigned long long)g_regs.rip, (unsigned long long)g_regs.eflags,
           (int)((g_regs.eflags >> 8) & 1), (int)((g_regs.eflags >> 10) & 1));
}

static int set_watch(uint64_t addr)
{
    int slot = -1;
    for (int i = 0; i < 4; i++)
        if (!g_watch[i]) { slot = i; break; }
    if (slot < 0) {
        printf("硬件观察点已满(最多 4 个)\n");
        return -1;
    }
    size_t dr = offsetof(struct user, u_debugreg);
    if (poke_user_off(dr + slot * sizeof(long), addr) != 0) {
        printf("写 DR%d 失败: %s\n", slot, strerror(errno));
        return -1;
    }
    uint64_t dr7 = peek_user_off(dr + 7 * sizeof(long));
    if (dr7 == (uint64_t)-1) dr7 = 0;
    dr7 |= 1ull << (2 * slot);          /* L{slot} 局部使能 */
    dr7 |= 0b01ull << (16 + 4 * slot);  /* RW=01 写 */
    dr7 |= 0b10ull << (18 + 4 * slot);  /* LEN=10 8 字节 */
    poke_user_off(dr + 7 * sizeof(long), dr7);
    poke_user_off(dr + 6 * sizeof(long), 0); /* 清 DR6 */
    g_watch[slot] = addr;
    printf("观察点 #%d @ 0x%llx (8 字节, 写触发)\n", slot, (unsigned long long)addr);
    return 0;
}

static void print_maps(void)
{
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/maps", g_child);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("无法读取 %s\n", path);
        return;
    }
    while (fgets(line, sizeof line, f))
        fputs(line, stdout);
    fclose(f);
}

static void print_bt(void)
{
    uint64_t frame = g_regs.rbp;
    printf("回溯(rbp 链, 需被调试程序 -fno-omit-frame-pointer):\n");
    for (int d = 0; d < 32 && frame; d++) {
        long w = peek_data(frame + 8);
        if ((w == -1 && errno) || w == 0)
            break;
        uint64_t ret = (uint64_t)w;
        const char *sym = sym_name_at(ret - 1); /* 返回地址通常指向 call 的下一条 */
        if (!sym)
            sym = sym_name_at(ret);
        printf("#%-2d 0x%llx  %s\n", d, (unsigned long long)ret, sym ? sym : "?");
        long nf = peek_data(frame);
        if ((nf == -1 && errno) || (uint64_t)nf <= frame)
            break;
        frame = (uint64_t)nf;
    }
}

static void cmd_examine(const char *fmt, const char *addr_s)
{
    /* x/Nxg addr | x/Nxb addr | x/s addr */
    uint64_t addr;
    if (parse_addr(addr_s, &addr) != 0) {
        printf("无法解析地址: %s\n", addr_s);
        return;
    }
    if (fmt[0] == 's' && !fmt[1]) {
        char buf[33];
        size_t bi = 0;
        for (int wi = 0; wi < 128; wi++) {
            long w = peek_data(addr + (uint64_t)wi * 8);
            if (w == -1 && errno) {
                printf("  读取失败 @ 0x%llx\n", (unsigned long long)(addr + (uint64_t)wi * 8));
                return;
            }
            char *b = (char *)&w;
            for (int k = 0; k < 8; k++) {
                if (b[k] == 0 || bi + 1 >= sizeof buf) {
                    buf[bi] = 0;
                    printf("0x%llx: \"%s\"\n", (unsigned long long)addr, buf);
                    return;
                }
                buf[bi++] = b[k];
            }
        }
        buf[bi] = 0;
        printf("0x%llx: \"%s\" (截断)\n", (unsigned long long)addr, buf);
        return;
    }
    char *end;
    long n = strtol(fmt, &end, 10);
    if (n <= 0) n = 1;
    if (n > 64) n = 64;
    if (strcmp(end, "xg") == 0) {
        for (long i = 0; i < n; i++) {
            long w = peek_data(addr + (uint64_t)i * 8);
            if (i % 2 == 0)
                printf("0x%llx:", (unsigned long long)(addr + (uint64_t)i * 8));
            if (w == -1 && errno)
                printf("  ????????????????");
            else
                printf("  0x%016lx", (unsigned long)w);
            putchar(i % 2 == 1 || i == n - 1 ? '\n' : ' ');
        }
    } else if (strcmp(end, "xb") == 0) {
        for (long i = 0; i < n; i++) {
            long w = peek_data(addr + (uint64_t)i); /* 逐字节读(取 8 字节低地址) */
            if (i % 16 == 0)
                printf("0x%llx:", (unsigned long long)(addr + (uint64_t)i));
            if (w == -1 && errno)
                printf(" ??");
            else
                printf(" %02x", (unsigned)((unsigned char *)&w)[0]);
            putchar(i % 16 == 15 || i == n - 1 ? '\n' : ' ');
        }
    } else {
        printf("格式应为 xg/xb/s, 例: x/8xg 0x400000 / x/s $rdi\n");
    }
}

static void kill_child(void)
{
    if (!g_dead && g_child > 0) {
        kill(g_child, SIGKILL);
        int st;
        waitpid(g_child, &st, 0);
    }
    g_dead = 1;
}

static void debug_help(void)
{
    puts("命令:");
    puts("  run [args...]          启动/重启被调试程序");
    puts("  continue (c)           继续运行");
    puts("  step (s)               单步一条指令");
    puts("  break (b) <sym|*addr>  设断点: b main / b *0x401100 / b *+0x20");
    puts("  delete (d) [n|all]     删除断点");
    puts("  info break             断点列表");
    puts("  regs                   寄存器");
    puts("  set $reg = value       修改寄存器");
    puts("  x/Nxg|Nxb|s <addr>     查看内存");
    puts("  watch <sym|addr>       硬件写观察点(≤4)");
    puts("  bt                     栈回溯(rbp 链)");
    puts("  maps                   进程内存映射");
    puts("  signal <n>             以信号 n 继续");
    puts("  kill / quit (q)        终止程序 / 退出");
}

static void set_prog_args(int argc, char **argv)
{
    /* argv[0]=prog, 其余为运行参数; 存入静态槽位供 execvp */
    size_t ai = 0;
    memset(g_argv_slots, 0, sizeof g_argv_slots);
    g_prog_args_buf[0] = 0;
    size_t used = 0;
    for (int i = 0; i < argc && ai + 1 < 64; i++) {
        size_t l = strlen(argv[i]) + 1;
        if (used + l > sizeof g_prog_args_buf) break;
        memcpy(g_prog_args_buf + used, argv[i], l);
        g_argv_slots[ai++] = g_prog_args_buf + used;
        used += l;
    }
    g_argv_slots[ai] = NULL;
    g_prog = g_argv_slots[0];
    g_prog_args = g_argv_slots;
}

int debug_main(int argc, char **argv)
{
    if (argc < 2 || arg_is(argv[1], "--help") || arg_is(argv[1], "-h")) {
        puts("用法: oskit debug <程序> [程序参数...]");
        puts("");
        puts("交互式调试器(命令从 stdin 逐行读取, 可管道脚本化):");
        puts("  printf 'b main\\nr\\nc\\nbt\\nq\\n' | oskit debug ./prog");
        debug_help();
        return argc < 2 ? 2 : 0;
    }
    set_prog_args(argc - 1, argv + 1);
    char line[512];
    int rc = 0;

    while (fgets(line, sizeof line, stdin)) {
        char *p = line;
        while (*p == ' ') p++;
        /* 去行尾 */
        char *e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;

        char *cmd = strtok(p, " ");
        char *a1 = strtok(NULL, " ");
        char *a2 = strtok(NULL, " ");
        char *a3 = strtok(NULL, " ");
        if (!cmd || !*cmd || cmd[0] == '#')
            continue;

        if (strcmp(cmd, "run") == 0 || strcmp(cmd, "r") == 0) {
            if (!g_dead) kill_child();
            launch(); /* 程序参数为启动 oskit 时给定, run 不换参 */
        } else if (strcmp(cmd, "continue") == 0 || strcmp(cmd, "c") == 0) {
            if (g_dead) { puts("程序未运行"); continue; }
            cont_run(0);
        } else if (strcmp(cmd, "signal") == 0) {
            if (g_dead || !a1) { puts("用法: signal <n>"); continue; }
            cont_run((int)strtol(a1, NULL, 0));
        } else if (strcmp(cmd, "step") == 0 || strcmp(cmd, "s") == 0) {
            if (g_dead) { puts("程序未运行"); continue; }
            step_one(g_last_sig);
            g_last_sig = 0;
            printf("rip=0x%llx\n", (unsigned long long)g_regs.rip);
        } else if (strcmp(cmd, "break") == 0 || strcmp(cmd, "b") == 0) {
            if (!a1) { puts("用法: break <符号|*地址>"); continue; }
            if (g_dead) { /* 程序未启动: 登记, run 后落实(需先算 PIE 基址) */
                if (g_n_pend < 32) {
                    snprintf(g_pending_bp[g_n_pend++], sizeof g_pending_bp[0], "%s", a1);
                    printf("[挂起] b %s (run 启动后生效)\n", a1);
                }
                continue;
            }
            try_break(a1);
        } else if (strcmp(cmd, "delete") == 0 || strcmp(cmd, "d") == 0) {
            if (a1 && strcmp(a1, "all") == 0) {
                for (int i = 0; i < MAX_BP; i++) g_bps[i] = (bp_t){ 0 };
                g_bp_step_addr = 0;
                puts("已清空断点");
            } else if (a1) {
                int n = atoi(a1);
                for (int i = 0; i < MAX_BP; i++)
                    if (g_bps[i].used && g_bps[i].num == n) {
                        if (g_bp_step_addr == g_bps[i].addr) {
                            bp_reinsert(g_bps[i].addr);
                            g_bp_step_addr = 0;
                        } else if (!g_dead)
                            bp_restore(g_bps[i].addr);
                        g_bps[i].used = 0;
                        printf("已删除断点 #%d\n", n);
                    }
            } else {
                puts("用法: delete <n>|all");
            }
        } else if (strcmp(cmd, "info") == 0) {
            if (a1 && strcmp(a1, "break") == 0) {
                for (int i = 0; i < MAX_BP; i++)
                    if (g_bps[i].used)
                        printf("#%d  0x%llx  %s\n", g_bps[i].num,
                               (unsigned long long)g_bps[i].addr, g_bps[i].label);
                for (int i = 0; i < 4; i++)
                    if (g_watch[i])
                        printf("观察点 #%d  0x%llx\n", i, (unsigned long long)g_watch[i]);
            } else {
                print_regs();
            }
        } else if (strcmp(cmd, "regs") == 0) {
            print_regs();
        } else if (strcmp(cmd, "set") == 0) {
            /* set $reg = value */
            if (!a1 || !a3 || a1[0] != '$') {
                puts("用法: set $reg = value");
                continue;
            }
            uint64_t v = strtoull(a3, NULL, 0);
            int ok = 0;
#define SR(nm) if (strcmp(a1 + 1, #nm) == 0) { g_regs.nm = v; ok = 1; }
            SR(rax) SR(rbx) SR(rcx) SR(rdx) SR(rsi) SR(rdi) SR(rbp) SR(rsp)
            SR(r8) SR(r9) SR(r10) SR(r11) SR(r12) SR(r13) SR(r14) SR(r15)
            SR(rip)
#undef SR
            if (ok && !g_dead) set_regs();
            printf(ok ? "%s = 0x%llx\n" : "未知寄存器 %s\n", ok ? a1 : a1,
                   ok ? (unsigned long long)v : 0ull);
        } else if (strncmp(cmd, "x/", 2) == 0) {
            /* GDB 风格: x/8xg <addr> / x/16xb <addr> / x/s <addr> */
            if (!a1) { puts("用法: x/Nxg|Nxb|s <addr>"); continue; }
            cmd_examine(cmd + 2, a1);
        } else if (strcmp(cmd, "watch") == 0) {
            if (g_dead) { puts("先 run 启动程序"); continue; }
            uint64_t addr;
            if (parse_addr(a1, &addr) != 0) { puts("无法解析地址"); continue; }
            set_watch(addr);
        } else if (strcmp(cmd, "bt") == 0) {
            print_bt();
        } else if (strcmp(cmd, "maps") == 0) {
            print_maps();
        } else if (strcmp(cmd, "kill") == 0) {
            kill_child();
            puts("已终止");
        } else if (strcmp(cmd, "help") == 0) {
            debug_help();
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            break;
        } else {
            printf("未知命令: %s (help 查看命令)\n", cmd);
        }
    }
    kill_child();
    if (g_elf) elf_close(g_elf);
    return rc;
}
