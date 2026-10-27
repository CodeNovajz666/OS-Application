/* main.c —— OSKit CLI 入口：子命令分发 / 全局帮助 / 版本 */
#include "common.h"

#include <stdio.h>
#include <string.h>

/* 六大工具入口（各文件自行处理 --help）*/
int perf_main(int argc, char **argv);
int functest_main(int argc, char **argv);
int vuln_main(int argc, char **argv);
int debug_main(int argc, char **argv);
int emu_main(int argc, char **argv);
int port_main(int argc, char **argv);

typedef struct {
    const char *name;
    const char *cn;        /* 中文名 */
    const char *summary;
    int (*fn)(int, char **);
} cmd_t;

static const cmd_t g_cmds[] = {
    { "perf",     "性能测试",  "微基准: CPU/内存/系统调用/进程线程/磁盘",   perf_main     },
    { "functest", "功能测试",  "Linux 系统调用与 POSIX 行为功能测试",      functest_main },
    { "vuln",     "漏洞分析",  "ELF 静态安全检查与危险函数扫描",           vuln_main     },
    { "debug",    "软件调试",  "ptrace 迷你调试器(断点/单步/观察点/回溯)", debug_main    },
    { "emu",      "系统模拟器", "RISC-V RV32I+M 指令集模拟器",             emu_main      },
    { "port",     "系统移植",  "裸机工程骨架/环境体检/QEMU 启动/镜像",     port_main     },
};

static void print_help(void)
{
    puts("OSKit " OSKIT_VERSION " —— 开源操作系统工具箱 (工作名, 名称待定)");
    puts("");
    puts("用法: oskit <子命令> [选项] [参数]");
    puts("");
    puts("子命令:");
    for (size_t i = 0; i < sizeof g_cmds / sizeof g_cmds[0]; i++)
        printf("  %-10s %-6s  %s\n", g_cmds[i].name, g_cmds[i].cn, g_cmds[i].summary);
    puts("");
    puts("通用: oskit help [子命令]    oskit version");
    puts("文档: docs/ 目录（每个工具一份技术文档）");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_help();
        return 2;
    }
    const char *a1 = argv[1];

    if (strcmp(a1, "version") == 0 || strcmp(a1, "--version") == 0) {
        printf("OSKit %s (C11/Linux, 六大工具: 性能测试/功能测试/漏洞分析/软件调试/系统模拟器/系统移植)\n",
               OSKIT_VERSION);
        return 0;
    }
    if (strcmp(a1, "help") == 0 || strcmp(a1, "--help") == 0) {
        if (argc >= 3) {
            /* help <cmd> 等价于 <cmd> --help */
            for (size_t i = 0; i < sizeof g_cmds / sizeof g_cmds[0]; i++) {
                if (strcmp(g_cmds[i].name, argv[2]) == 0) {
                    const char *av[] = { g_cmds[i].name, "--help", NULL };
                    return g_cmds[i].fn(2, (char **)av);
                }
            }
            fprintf(stderr, "oskit: 未知子命令: %s\n", argv[2]);
            return 2;
        }
        print_help();
        return 0;
    }

    for (size_t i = 0; i < sizeof g_cmds / sizeof g_cmds[0]; i++) {
        if (strcmp(g_cmds[i].name, a1) == 0)
            return g_cmds[i].fn(argc - 1, argv + 1);
    }
    fprintf(stderr, "oskit: 未知子命令: %s（oskit help 查看用法）\n", a1);
    return 2;
}
