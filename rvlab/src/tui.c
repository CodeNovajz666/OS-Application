/* tui.c —— TUI 工作台：原始 ANSI 终端(无 ncurses 依赖)
 * 布局: 标题栏 / [文件 | 编辑器 | 寄存器+内存] / 控制台 / 命令行 / keybar */
#include "tui.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "analyze.h"

/* ---------- ANSI 助手 ---------- */
#define ESC "\x1b"
static void cur_pos(int row, int col) { printf(ESC "[%d;%dH", row + 1, col + 1); }
static void cls(void)                 { fputs(ESC "[2J", stdout); }
static void alt_on(void)              { fputs(ESC "[?1049h" ESC "[?25l", stdout); }
static void alt_off(void)             { fputs(ESC "[?25h" ESC "[?1049l", stdout); }
#define C_DIM   ESC "[90m"
#define C_DEF   ESC "[0m"
#define C_ACC   ESC "[36m"
#define C_KEY   ESC "[33m"
#define C_ERR   ESC "[31m"
#define C_OK    ESC "[32m"
#define C_SEL   ESC "[7m"
#define C_STR   ESC "[35m"
#define C_NUM   ESC "[34m"

/* 按显示宽度截断 UTF-8(粗略: 按字节截, 遇续字节回退) */
static void trunc_disp(const char *s, size_t w, char *out, size_t outsz)
{
    size_t bi = 0, disp = 0, last = 0;
    while (s[bi] && disp < w && bi + 1 < outsz) {
        unsigned char c = (unsigned char)s[bi];
        size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (s[bi + len - 1] == 0 && len > 1)
            len = 1;
        for (size_t k = 0; k < len && s[bi]; k++)
            out[bi + k] = s[bi + k];
        bi += len;
        disp += c >= 0xe0 ? 2 : 1;
        last = bi;
    }
    out[last] = 0;
}

/* 在 (row,col) 起 width 列内输出(截断+空格补齐, 不越界破坏边框) */
static void put_row(int row, int col, size_t width, const char *s)
{
    char t[1024];
    trunc_disp(s ? s : "", width, t, sizeof t);
    cur_pos(row, col);
    fputs(t, stdout);
    for (size_t d = strlen(t); d < width; d++)
        fputc(' ', stdout);
}

static void put_rowc(int row, int col, size_t width, const char *color, const char *s)
{
    char t[900];
    trunc_disp(s ? s : "", width, t, sizeof t);
    cur_pos(row, col);
    fputs(color, stdout);
    fputs(t, stdout);
    fputs(C_DEF, stdout);
    for (size_t d = strlen(t); d < width; d++)
        fputc(' ', stdout);
}

static void draw_box(int row, int col, int h, int w, const char *title, int focus)
{
    cur_pos(row, col);
    fputs(focus ? C_ACC : C_DIM, stdout);
    fputs("┌", stdout);
    for (int i = 0; i < w - 2; i++)
        fputs("─", stdout);
    fputs("┐", stdout);
    for (int r = 1; r < h - 1; r++) {
        cur_pos(row + r, col);
        fputs("│", stdout);
        cur_pos(row + r, col + w - 1);
        fputs("│", stdout);
    }
    cur_pos(row + h - 1, col);
    fputs("└", stdout);
    for (int i = 0; i < w - 2; i++)
        fputs("─", stdout);
    fputs("┘", stdout);
    if (title && *title) {
        char t[128];
        snprintf(t, sizeof t, " %s ", title);
        cur_pos(row, col + 2);
        fputs(t, stdout);
    }
    fputs(C_DEF, stdout);
    /* 面板内部清空 */
    for (int r = 1; r < h - 1; r++) {
        cur_pos(row + r, col + 1);
        printf(ESC "[%dX", w - 2);
    }
}

/* ---------- 终端原始模式 ---------- */
static struct termios g_tio_save;
static int g_tio_saved;

static int term_raw(void)
{
    if (tcgetattr(0, &g_tio_save) != 0)
        return -1;
    g_tio_saved = 1;
    struct termios t = g_tio_save;
    t.c_lflag &= ~(ECHO | ICANON | ISIG);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    return tcsetattr(0, TCSANOW, &t);
}

static void term_restore(void)
{
    if (g_tio_saved)
        tcsetattr(0, TCSANOW, &g_tio_save);
}

static void term_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) != 0 || ws.ws_row < 5) {
        *rows = 24;
        *cols = 80;
        return;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
}

/* ---------- 文件列表面板 ---------- */
#define FL_MAX 200
typedef struct {
    char names[FL_MAX][80];
    int  n, sel;
} filelist_t;

static void fl_refresh(filelist_t *fl)
{
    fl->n = 0;
    DIR *d = opendir(".");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && fl->n < FL_MAX) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(fl->names[fl->n++], 80, "%.70s", e->d_name);
    }
    closedir(d);
    /* 简单冒泡排序 */
    for (int i = 0; i < fl->n; i++)
        for (int j = i + 1; j < fl->n; j++)
            if (strcmp(fl->names[i], fl->names[j]) > 0) {
                char t[80];
                strcpy(t, fl->names[i]);
                strcpy(fl->names[i], fl->names[j]);
                strcpy(fl->names[j], t);
            }
    if (fl->sel >= fl->n)
        fl->sel = fl->n ? fl->n - 1 : 0;
}

/* ---------- 语法高亮(逐 token 分类) ---------- */
static const char *C_KW[] = {
    "int", "char", "void", "long", "short", "unsigned", "signed", "static",
    "const", "if", "else", "for", "while", "do", "return", "switch", "case",
    "break", "continue", "struct", "typedef", "enum", "sizeof", "volatile",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "size_t", NULL,
};
static const char *A_KW[] = {
    "lui", "auipc", "jal", "jalr", "beq", "bne", "blt", "bge", "bltu", "bgeu",
    "lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw", "addi", "slti", "sltiu",
    "xori", "ori", "andi", "slli", "srli", "srai", "add", "sub", "sll", "slt",
    "sltu", "xor", "srl", "sra", "or", "and", "mul", "mulh", "div", "divu",
    "rem", "remu", "fence", "ecall", "ebreak", "call", "li", "la", "mv",
    "ret", "j", "nop", "wfi", NULL,
};

static const char *tok_color(const char *tok, int asm_mode)
{
    if (!*tok)
        return C_DEF;
    if (asm_mode) {
        for (const char **k = A_KW; *k; k++)
            if (strcmp(tok, *k) == 0)
                return C_KEY;
        if (tok[0] == 'x' && tok[1] >= '0' && tok[1] <= '9')
            return C_NUM;
        if ((tok[0] == '.' && tok[1]) || (tok[0] == 'r' && tok[1] >= '0'))
            return C_ACC;
        return C_DEF;
    }
    for (const char **k = C_KW; *k; k++)
        if (strcmp(tok, *k) == 0)
            return C_KEY;
    if (tok[0] == '"' || tok[0] == '\'')
        return C_STR;
    if ((tok[0] >= '0' && tok[0] <= '9') ||
        (tok[0] == '-' && tok[1] >= '0' && tok[1] <= '9') ||
        (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')))
        return C_NUM;
    if (tok[0] == '#')
        return C_OK;
    return C_DEF;
}

static void draw_code_line(int row, int col, size_t width, const char *text, int asm_mode)
{
    char t[600];
    trunc_disp(text, width, t, sizeof t);
    /* 注释部分整体变暗绿 */
    const char *cmt = NULL;
    if (asm_mode)
        cmt = strchr(t, '#');
    else
        cmt = strstr(t, "//");
    if (!cmt && !asm_mode)
        cmt = strstr(t, "/*");
    cur_pos(row, col);
    size_t cmt_off = cmt ? (size_t)(cmt - t) : strlen(t);
    /* 代码段逐 token 着色 */
    char seg[600];
    size_t so = 0;
    while (so < cmt_off) {
        size_t to = so;
        while (to < cmt_off && t[to] != ' ' && t[to] != '\t' &&
               t[to] != ',' && t[to] != '(' && t[to] != ')' &&
               t[to] != '[' && t[to] != ']' && t[to] != '{' && t[to] != '}' &&
               t[to] != ';' && t[to] != '+' && t[to] != '-' && t[to] != '=')
            to++;
        if (to > so) {
            size_t l = to - so;
            memcpy(seg, t + so, l);
            seg[l] = 0;
            fputs(tok_color(seg, asm_mode), stdout);
            fputs(seg, stdout);
            fputs(C_DEF, stdout);
        }
        if (to < cmt_off) {
            fputc(t[to], stdout);
            to++;
        }
        so = to;
    }
    if (cmt) {
        fputs(C_OK, stdout);
        fputs(cmt, stdout);
        fputs(C_DEF, stdout);
    }
}

/* ---------- 键读取 ---------- */
#define K_UP 1000
#define K_DOWN 1001
#define K_LEFT 1002
#define K_RIGHT 1003
#define K_PGUP 1004
#define K_PGDN 1005
#define K_HOME 1006
#define K_END 1007
#define K_DEL 1008
#define K_ESC 1009
#define K_TAB 1010
#define K_ENTER 1011
#define K_BS 1012
/* Ctrl 组合键归一化: 到达字节是 ch&0x1f(如 Ctrl-K=0x0b), 统一映射后比较 */
#define K_CTRL(ch) (0x100 + ((ch) & 0x1f))

static int read_key(void)
{
    unsigned char b[8];
    ssize_t n = read(0, b, 1);
    if (n <= 0)
        return 'q';
    if (b[0] == 0x1b) {
        n = read(0, b + 1, 5);
        if (n <= 0)
            return K_ESC;
        if (b[1] == '[' || b[1] == 'O') {
            switch (b[2]) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            case 'F': return K_END;
            case '3': return K_DEL;
            case '5': return K_PGUP;
            case '6': return K_PGDN;
            default: return K_ESC;
            }
        }
        return K_ESC;
    }
    if (b[0] == '\t')
        return K_TAB;
    if (b[0] == '\n' || b[0] == '\r')
        return K_ENTER;
    if (b[0] == 0x7f || b[0] == 0x08)
        return K_BS;
    if (b[0] < 0x20)
        return K_CTRL(b[0]);
    return b[0];                                   /* 含 UTF-8 逐字节 */
}

/* ---------- 主循环 ---------- */
enum { F_FILES = 0, F_EDIT, F_CONSOLE, F_MEM, F_N };
static const char *F_NAME[F_N] = { "文件", "编辑", "控制台", "内存" };

void tui_run(app_t *a)
{
    if (term_raw() != 0)
        return;
    a->tui_mode = 1;
    alt_on();
    cls();

    filelist_t fl = { 0 };
    fl_refresh(&fl);
    int focus = F_EDIT;
    int mode_cmd = 0;                              /* 0 正常 1 命令行输入 */
    char cmdline[180] = { 0 };
    size_t cmdlen = 0;
    uint32_t prev_regs[32];
    memset(prev_regs, 0xAA, sizeof prev_regs);
    int prev_pc = -1;

    while (!a->quit) {
        int rows, cols;
        term_size(&rows, &cols);
        if (rows < 24 || cols < 70) {
            cls();
            cur_pos(1, 0);
            printf("终端过小 (%dx%d), 需要 >= 70x24。按 q 退出。", cols, rows);
            fflush(stdout);
            if (read_key() == 'q')
                break;
            continue;
        }

        /* 布局(0 基行号):
         * 0 标题 · 1..main_h+1 主区(main_h-1 行内容) · main_h+2..main_h+8 控制台
         * rows-2 命令行 · rows-1 keybar                                   */
        int fl_w = 22, right_w = 34;
        int main_h = rows - 11;
        int con_h = 5;
        int ed_w = cols - fl_w - right_w - 1;
        if (ed_w < 20)
            ed_w = 20;

        /* 标题栏 */
        char title[256];
        snprintf(title, sizeof title,
                 " RVLab ── %.150s%.2s │ %.12s │ 指令 %llu",
                 a->progname[0] ? a->progname : "(未装载)",
                 a->ed.dirty ? " *" : "",
                 a->vm ? (a->vm->exited ? "已退出" : "已停止") : "空闲",
                 a->vm ? (unsigned long long)a->vm->instret : 0ull);
        cur_pos(0, 0);
        fputs(ESC "[7m", stdout);
        put_row(0, 0, (size_t)cols, title);
        fputs(ESC "[K" C_DEF, stdout);

        /* 文件面板 */
        draw_box(1, 0, main_h + 1, fl_w, "文件(r 刷新)", focus == F_FILES);
        int fl_rows = main_h - 1;
        for (int i = 0; i < fl_rows && i < fl.n; i++) {
            if (i == fl.sel && focus == F_FILES)
                put_rowc(2 + i, 2, (size_t)fl_w - 4, C_SEL, fl.names[i]);
            else
                put_row(2 + i, 2, (size_t)fl_w - 4, fl.names[i]);
        }
        if (!fl.n)
            put_rowc(2, 2, (size_t)fl_w - 4, C_DIM, "(空目录)");

        /* 编辑器/仪表板 */
        draw_box(1, fl_w - 1, main_h + 1, ed_w + 2,
                 a->ed.path[0] ? a->ed.path : "编辑器", focus == F_EDIT);
        if (a->ed.path[0]) {
            const char *ext = strrchr(a->ed.path, '.');
            int asm_mode = ext && (strcmp(ext, ".s") == 0 || strcmp(ext, ".S") == 0);
            if (a->ed.cy < a->ed.scroll)
                a->ed.scroll = a->ed.cy;
            if (a->ed.cy >= a->ed.scroll + (size_t)(main_h - 1))
                a->ed.scroll = a->ed.cy - (size_t)(main_h - 1) + 1;
            for (int i = 0; i < main_h - 1; i++) {
                size_t ln = a->ed.scroll + (size_t)i;
                char num[16];
                snprintf(num, sizeof num, "%4zu ", ln + 1);
                put_rowc(2 + i, fl_w, 5, ln < a->ed.n ? C_DIM : C_DEF, num);
                if (ln < a->ed.n) {
                    draw_code_line(2 + i, fl_w + 6, (size_t)ed_w - 7,
                                   buf_line(&a->ed, ln), asm_mode);
                }
            }
            /* 编辑器光标 */
            if (focus == F_EDIT && !mode_cmd) {
                cur_pos(2 + (int)(a->ed.cy - a->ed.scroll),
                        fl_w + 6 + (int)a->ed.cx);
                fputs(ESC "[?25h", stdout);
            }
        } else {
            const char *dash[] = {
                "RVLab —— RISC-V 系统开发工作台",
                "",
                "  Tab 切面板 · : 命令行 · ^S 存 ^B 建 ^R 跑 ^N 步 ^A 析 ^K 退",
                "",
                "  开始: :demo 1      载入内置演示程序",
                "        :skeleton riscv64 myos   生成裸机工程骨架",
                "        :edit <file>   编辑 · :build 交叉构建",
                "        :open <elf>    装载 ELF · :bp/:run/:step 调试",
                "        :analyze/:harden/:stats  分析(--json 可给 Agent)",
                "",
                "  headless: rvlab --script cmds.txt / 管道 stdin",
                "  GDB: rvlab --gdbstub 1234 xx.elf → target remote :1234",
            };
            for (size_t i = 0; i < sizeof dash / sizeof dash[0] && (int)i < main_h - 1; i++)
                put_row(2 + (int)i, fl_w, (size_t)ed_w - 2, dash[i]);
        }

        /* 寄存器 + 内存(右列: 寄存器 11 行盒 + 内存盒到底) */
        draw_box(1, fl_w + ed_w, 11, right_w, "寄存器", 0);
        if (a->vm) {
            char line[128];
            for (int r = 0; r < 32; r += 4) {
                snprintf(line, sizeof line, "%3s=%08x %3s=%08x %3s=%08x %3s=%08x",
                         rv_reg_name(r), a->vm->x[r],
                         rv_reg_name(r + 1), a->vm->x[r + 1],
                         rv_reg_name(r + 2), a->vm->x[r + 2],
                         rv_reg_name(r + 3), a->vm->x[r + 3]);
                int changed = 0;
                for (int k = 0; k < 4; k++)
                    if (prev_regs[r + k] != a->vm->x[r + k])
                        changed = 1;
                put_rowc(2 + r / 4, fl_w + ed_w + 1, (size_t)right_w - 2,
                         changed ? C_KEY : C_DEF, line);
            }
            snprintf(line, sizeof line, " pc=%08x %s instret=%llu",
                     a->vm->pc, a->vm->exited ? "退出" : "",
                     (unsigned long long)a->vm->instret);
            put_rowc(10, fl_w + ed_w + 1, (size_t)right_w - 2,
                     prev_pc != (int)a->vm->pc ? C_KEY : C_DEF, line);
        } else {
            put_rowc(2, fl_w + ed_w + 1, (size_t)right_w - 2, C_DIM, "(未装载, :demo 或 :open)");
        }
        prev_pc = a->vm ? (int)a->vm->pc : -1;
        if (a->vm)
            memcpy(prev_regs, a->vm->x, sizeof prev_regs);

        int mem_top = 12;
        int mem_h = main_h + 1 - 11;
        if (mem_h < 3)
            mem_h = 3;
        draw_box(mem_top, fl_w + ed_w, mem_h, right_w,
                 "内存(↑↓±0x40 ←→±4 g跳转)", focus == F_MEM);
        if (a->vm) {
            int mrows = mem_h - 2;
            for (int i = 0; i < mrows; i++) {
                uint32_t addr = a->mem_addr + (uint32_t)(i * 8);
                uint8_t b[8];
                char line[128];
                if (rv_peek(a->vm, addr, b, 8) == 0) {
                    char hex[32] = "";
                    for (int k = 0; k < 8; k++)
                        snprintf(hex + k * 4, 5, "%02x ", b[k]);
                    snprintf(line, sizeof line, "%08x %s", addr, hex);
                } else
                    snprintf(line, sizeof line, "%08x ?? ..", addr);
                put_row(mem_top + 1 + i, fl_w + ed_w + 1, (size_t)right_w - 2, line);
            }
        }

        /* 控制台 */
        int con_top = main_h + 2;
        draw_box(con_top, 0, con_h + 2, cols, "控制台(运行输出/跟踪/命令回显)",
                 focus == F_CONSOLE);
        {
            int shown = 0;
            int skip = a->con_scroll;              /* 距底部偏移 */
            for (int i = a->con_count; i > 0 && shown < con_h; i--) {
                if (skip > 0) { skip--; continue; }
                int idx = (a->con_head - i + CON_LINES * 2) % CON_LINES;
                put_row(con_top + 1 + (con_h - 1 - shown), 2, (size_t)cols - 4,
                        a->con[idx]);
                shown++;
            }
        }

        /* 命令行 + keybar */
        if (mode_cmd) {
            cur_pos(rows - 2, 0);
            fputs(C_ACC ":" C_DEF, stdout);
            fputs(cmdline, stdout);
            fputs("_", stdout);
            fputs(ESC "[K", stdout);
            put_rowc(rows - 1, 0, (size_t)cols, C_DIM,
                     "Enter 执行 · Esc 取消");
        } else {
            put_row(rows - 2, 0, (size_t)cols, "");
            char kb[256];
            snprintf(kb, sizeof kb,
                     " Tab:%s · : 命令 · ^S 存 · ^B 建 · ^R 跑 · ^N 步 · ^A 析 · ^K 退出",
                     F_NAME[focus]);
            put_rowc(rows - 1, 0, (size_t)cols, C_DIM, kb);
        }
        if (!(focus == F_EDIT && !mode_cmd))
            fputs(ESC "[?25l", stdout);
        fflush(stdout);

        /* ---------- 输入 ---------- */
        int k = read_key();
        if (mode_cmd) {
            if (k == K_ENTER) {
                cmdline[cmdlen] = 0;
                if (cmdlen) {
                    app_con(a, "> %s", cmdline);
                    cmd_exec(a, cmdline);
                }
                mode_cmd = 0;
                cmdlen = 0;
            } else if (k == K_ESC) {
                mode_cmd = 0;
                cmdlen = 0;
            } else if (k == K_BS) {
                if (cmdlen) cmdline[--cmdlen] = 0;
            } else if (k >= 0x20 && k < 0x100 && cmdlen + 1 < sizeof cmdline) {
                cmdline[cmdlen++] = (char)k;
                cmdline[cmdlen] = 0;
            }
            continue;
        }

        /* 全局键 */
        if (k == K_CTRL('k')) {
            a->quit = 1;
        } else if (k == K_CTRL('s')) {
            cmd_exec(a, (char *)"w");
        } else if (k == K_CTRL('b')) {
            cmd_exec(a, (char *)"build");
        } else if (k == K_CTRL('r')) {
            cmd_exec(a, (char *)"run");
        } else if (k == K_CTRL('n')) {
            cmd_exec(a, (char *)"step");
        } else if (k == K_CTRL('a')) {
            cmd_exec(a, (char *)"analyze");
        } else if (k == K_CTRL('o')) {
            mode_cmd = 1;
            cmdlen = 5;
            strcpy(cmdline, "open ");
        } else if (k == ':' || k == '/') {
            mode_cmd = 1;
            cmdlen = 0;
            cmdline[0] = 0;
        } else if (k == K_TAB) {
            focus = (focus + 1) % F_N;
            if (focus == F_FILES)
                fl_refresh(&fl);
        } else if (k == K_ESC) {
            /* 无操作 */
        } else {
            /* 面板键 */
            if (focus == F_FILES) {
                if (k == K_UP && fl.sel > 0) fl.sel--;
                else if (k == K_DOWN && fl.sel + 1 < fl.n) fl.sel++;
                else if (k == K_ENTER && fl.n) {
                    char cmd3[128], *ext = strrchr(fl.names[fl.sel], '.');
                    if (ext && strcmp(ext, ".elf") == 0)
                        snprintf(cmd3, sizeof cmd3, "open %s", fl.names[fl.sel]);
                    else
                        snprintf(cmd3, sizeof cmd3, "edit %s", fl.names[fl.sel]);
                    cmd_exec(a, cmd3);
                } else if (k == 'r' || k == 'R')
                    fl_refresh(&fl);
            } else if (focus == F_EDIT) {
                if (a->ed.path[0]) {
                    if (k >= 0x20 && k < 0x100)
                        buf_insert(&a->ed, (char)k);
                    else if (k == K_ENTER)
                        buf_enter(&a->ed);
                    else if (k == K_BS)
                        buf_backspace(&a->ed);
                    else if (k == K_DEL)
                        buf_delete(&a->ed);
                    else if (k == K_UP) buf_arrow(&a->ed, 0, (size_t)main_h);
                    else if (k == K_DOWN) buf_arrow(&a->ed, 1, (size_t)main_h);
                    else if (k == K_LEFT) buf_arrow(&a->ed, 2, 0);
                    else if (k == K_RIGHT) buf_arrow(&a->ed, 3, 0);
                    else if (k == K_HOME) buf_arrow(&a->ed, 4, 0);
                    else if (k == K_END) buf_arrow(&a->ed, 5, 0);
                    else if (k == K_PGUP) buf_arrow(&a->ed, 6, (size_t)main_h);
                    else if (k == K_PGDN) buf_arrow(&a->ed, 7, (size_t)main_h);
                }
            } else if (focus == F_CONSOLE) {
                if (k == K_PGUP || k == K_UP)
                    a->con_scroll += 5;
                else if (k == K_PGDN || k == K_DOWN)
                    a->con_scroll = a->con_scroll >= 5 ? a->con_scroll - 5 : 0;
            } else if (focus == F_MEM) {
                if (k == K_UP) a->mem_addr -= 0x40;
                else if (k == K_DOWN) a->mem_addr += 0x40;
                else if (k == K_LEFT) a->mem_addr -= 4;
                else if (k == K_RIGHT) a->mem_addr += 4;
                else if (k == 'g' || k == 'G') {
                    mode_cmd = 1;
                    cmdlen = 4;
                    strcpy(cmdline, "mem ");
                }
            }
        }
    }

    alt_off();
    term_restore();
    a->tui_mode = 0;
}
