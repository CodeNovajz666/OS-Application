/* cmd.h —— 命令中枢：TUI 命令行 / --script / 管道 stdin 共用同一解释器 */
#ifndef RVLAB_CMD_H
#define RVLAB_CMD_H

#include <stddef.h>
#include <stdint.h>
#include "rv.h"
#include "buf.h"

#define CON_LINES 400
#define CON_COLS  200

struct elf_file;

typedef struct app {
    rv_vm *vm;
    void  *elfbuf;                 /* 程序文件内容(装载来源) */
    size_t elflen;
    struct elf_file *ef;           /* 符号查询用 */
    char   progname[256];          /* 显示名 */
    uint32_t entry;
    rv_sym *syms;
    size_t  nsyms;

    int      trace_on;
    int      quit;
    uint32_t mem_addr;
    uint64_t run_max;
    int      tui_mode;             /* TUI 下为 1(禁用阻塞式 gdb 命令) */

    buf_t ed;                      /* 编辑缓冲 */

    /* 控制台环形缓冲(emit 为 NULL 时 TUI 由此渲染) */
    char con[CON_LINES][CON_COLS];
    int  con_head, con_count;
    int  con_scroll;               /* >0: 距底部偏移(TUI 翻页用) */
    char uart_cur[CON_COLS];       /* UART 半行累积 */
    int  uart_n;

    void (*emit)(void *ud, const char *line);   /* headless: 直接写 stdout */
    void  *emit_ud;
} app_t;

void app_con(app_t *a, const char *fmt, ...);
void app_con_block(app_t *a, const char *text);  /* 多行文本逐行入控制台 */
void app_uart_cb(void *ud, uint8_t b);
void app_trace_cb(void *ud, uint32_t pc, uint32_t inst);

int  app_open_prog(app_t *a, const char *path);  /* 0 成功 */
void app_load_demo(app_t *a, int which);
void app_unload(app_t *a);

/* 执行一条命令行; 返回 0 成功(供脚本判错) */
int cmd_exec(app_t *a, char *line);
void cmd_help(app_t *a);

#endif
