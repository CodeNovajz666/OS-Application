/* main.c —— RVLab 入口：TUI / --script / 管道 headless / --gdbstub */
#include "cmd.h"
#include "gdbstub.h"
#include "tui.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define RVLAB_VERSION "0.1.0"

static void line_emit(void *ud, const char *line)
{
    (void)ud;
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void usage(void)
{
    puts("RVLab " RVLAB_VERSION " —— RISC-V 系统开发工作台(TUI IDE)");
    puts("");
    puts("用法: rvlab [选项] [程序.elf]");
    puts("");
    puts("  (无参数, 终端)      进入 TUI 工作台");
    puts("  --demo <1|2>        载入内置演示程序(1: EMU OK/exit42, 2: fib(10)=55)");
    puts("  --script <文件>     headless: 逐行执行命令文件(AI Agent/CI 入口)");
    puts("  --gdbstub <端口>    GDB RSP 服务模式(gdb: target remote :端口)");
    puts("  --help | --version");
    puts("");
    puts("管道即 headless: printf 'demo 2\\nrun\\nquit\\n' | rvlab");
}

int main(int argc, char **argv)
{
    const char *script = NULL, *prog = NULL;
    int gdb_port = 0, demo = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("RVLab %s (C11/Linux, 复用 oskit 内核: RV32 VM/ELF/加固规则)\n",
                   RVLAB_VERSION);
            return 0;
        } else if (strncmp(argv[i], "--script=", 9) == 0) {
            script = argv[i] + 9;
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            script = argv[++i];
        } else if (strncmp(argv[i], "--gdbstub=", 10) == 0) {
            gdb_port = atoi(argv[i] + 10);
        } else if (strcmp(argv[i], "--gdbstub") == 0 && i + 1 < argc) {
            gdb_port = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--demo=", 7) == 0) {
            demo = atoi(argv[i] + 7);
        } else if (strcmp(argv[i], "--demo") == 0 && i + 1 < argc) {
            demo = atoi(argv[++i]);
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "rvlab: 未知选项 %s\n", argv[i]);
            return 2;
        } else {
            prog = argv[i];
        }
    }

    app_t app;
    memset(&app, 0, sizeof app);
    app.run_max = 50000000ull;
    app.emit = line_emit;                     /* headless 输出钩子(TUI 会置 NULL) */
    app.emit_ud = NULL;

    if (prog && app_open_prog(&app, prog) != 0)
        return 1;
    if (demo)
        app_load_demo(&app, demo == 2 ? 2 : 1);

    if (gdb_port > 0) {
        app.emit = NULL;                      /* RSP 模式安静 */
        return gdbstub_serve(&app, gdb_port);
    }

    if (script) {
        FILE *f = fopen(script, "r");
        if (!f) {
            fprintf(stderr, "rvlab: 无法打开脚本 %s\n", script);
            return 1;
        }
        char line[512];
        while (!app.quit && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (app.emit) {
                printf("> %s\n", line);
                fflush(stdout);
            }
            cmd_exec(&app, line);
        }
        fclose(f);
        return 0;
    }

    if (isatty(0)) {
        app.emit = NULL;                      /* TUI: 输出只进控制台环形 */
        tui_run(&app);
        return 0;
    }

    /* 管道 headless: 逐行读 stdin */
    char line[512];
    while (!app.quit && fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        printf("> %s\n", line);
        fflush(stdout);
        cmd_exec(&app, line);
    }
    return 0;
}
