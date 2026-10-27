/* port.c —— 系统移植：环境体检 / 裸机工程骨架生成 / QEMU 启动 / 磁盘镜像
 * 设计见 docs/06-port.md。目标机 = 一行 target_desc + 模板, 新增机型只加一行。 */
#include "common.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------- 目标机描述 ---------- */
typedef struct {
    const char *target;      /* riscv32/riscv64/armv7/aarch64/i386 */
    const char *cn;
    const char *qemu;        /* qemu-system-<arch> */
    const char *machine;     /* -M 值 */
    const char *qemu_extra;  /* 附加参数(如 -bios none) */
    const char *cross;       /* 默认 CROSS 前缀(空=宿主 gcc -m32) */
    const char *ram_base, *ram_len;
    const char *arch_flag;   /* Makefile 额外 CFLAGS */
    int uart;                /* 0=NS16550 1=PL011 2=x86 COM1 */
} target_desc;

enum { UART_NS16550 = 0, UART_PL011 = 1, UART_X86COM = 2 };

static const target_desc g_targets[] = {
    { "riscv32", "RISC-V 32位", "qemu-system-riscv32", "virt", "-bios none",
      "riscv32-unknown-elf-", "0x80000000", "128M", "", UART_NS16550 },
    { "riscv64", "RISC-V 64位", "qemu-system-riscv64", "virt", "-bios none",
      "riscv64-unknown-elf-", "0x80000000", "128M", "", UART_NS16550 },
    { "armv7",   "ARM 32位",    "qemu-system-arm",     "virt", "",
      "arm-none-eabi-",      "0x40000000", "128M", "", UART_PL011 },
    { "aarch64", "ARM 64位",    "qemu-system-aarch64", "virt", "-cpu cortex-a57",
      "aarch64-linux-gnu-",  "0x40000000", "128M", "", UART_PL011 },
    { "i386",    "x86 32位",    "qemu-system-i386",    "", "",
      "",                     "0x100000",   "128M", "-m32", UART_X86COM },
};

#define NTGT (sizeof g_targets / sizeof g_targets[0])

static const target_desc *find_target(const char *t)
{
    for (size_t i = 0; i < NTGT; i++)
        if (strcmp(g_targets[i].target, t) == 0)
            return &g_targets[i];
    return NULL;
}

/* ===================================================================== */
/* check —— 交叉编译/QEMU 环境体检                                         */
/* ===================================================================== */
static const char *g_check_tools[] = {
    "gcc", "g++", "make", "gdb", "python3",
    "qemu-system-x86_64", "qemu-system-i386",
    "qemu-system-riscv32", "qemu-system-riscv64",
    "qemu-system-arm", "qemu-system-aarch64",
    "riscv64-unknown-elf-gcc", "riscv64-linux-gnu-gcc",
    "riscv32-unknown-elf-gcc", "arm-none-eabi-gcc",
    "aarch64-linux-gnu-gcc", "xorriso", "grub-mkrescue",
};

static void port_usage(void);

static int cmd_check(int argc, char **argv)
{
    int json = 0;
    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "--json")) json = 1;
        else { port_usage(); return 2; }
    }
    if (json) putchar('[');
    else puts("== OSKit 交叉编译环境体检 ==");
    int first = 1, n_ok = 0;
    for (size_t i = 0; i < sizeof g_check_tools / sizeof g_check_tools[0]; i++) {
        const char *t = g_check_tools[i];
        char cmd[256], ver[160] = "";
        snprintf(cmd, sizeof cmd,
                 "command -v %s >/dev/null 2>&1 && %s --version 2>/dev/null | head -1", t, t);
        FILE *p = popen(cmd, "r");
        int ok = (p && fgets(ver, sizeof ver, p) != NULL);
        if (p) pclose(p);
        if (ok) n_ok++;
        /* 去掉行尾换行 */
        ver[strcspn(ver, "\r\n")] = 0;
        if (json) {
            printf("%s{\"tool\":\"%s\",\"ok\":%s,\"version\":",
                   first ? "" : ",", t, ok ? "true" : "false");
            json_escape(stdout, ok ? ver : "");
            putchar('}');
        } else {
            printf("  [%s] %-26s %s\n", ok ? "OK" : "--", t, ok ? ver : "未安装");
        }
        first = 0;
    }
    if (json) {
        puts("]");
    } else {
        printf("共 %zu 项, %d 项可用\n"
               "提示: oskit port gen 生成的工程按 target 提示所需工具链; qemu 缺失时 run --dry 仍可验证命令行\n",
               sizeof g_check_tools / sizeof g_check_tools[0], n_ok);
    }
    return 0;
}

/* ===================================================================== */
/* gen —— 裸机工程骨架                                                     */
/* ===================================================================== */
static int write_file(const char *dir, const char *name, const char *content)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "oskit port: 无法写入 %s: %s\n", path, strerror(errno));
        return -1;
    }
    fputs(content, f);
    fclose(f);
    return 0;
}

static const char *tpl_crt0_riscv(void)
{
    return
        "/* 启动代码: 设全局指针与栈, 进入内核入口 */\n"
        "    .section .text.init\n"
        "    .globl _start\n"
        "_start:\n"
        "    la   gp, __global_pointer$\n"
        "    la   sp, __stack_top\n"
        "    call kmain\n"
        "1:  wfi\n"
        "    j    1b\n";
}

static const char *tpl_crt0_aarch64(void)
{
    return
        "/* 启动代码: 设栈, 进入内核入口 */\n"
        "    .section .text.init\n"
        "    .globl _start\n"
        "_start:\n"
        "    ldr  x30, =__stack_top\n"
        "    mov  sp, x30\n"
        "    bl   kmain\n"
        "1:  wfe\n"
        "    b    1b\n";
}

static const char *tpl_crt0_armv7(void)
{
    return
        "/* 启动代码: 设栈, 进入内核入口 */\n"
        "    .section .text.init\n"
        "    .globl _start\n"
        "_start:\n"
        "    ldr  r0, =__stack_top\n"
        "    mov  sp, r0\n"
        "    bl   kmain\n"
        "1:  wfe\n"
        "    b    1b\n";
}

static const char *tpl_crt0_i386(void)
{
    return
        "/* 启动代码: Multiboot 头(须在文件前 8KiB) + 栈 + 内核入口 */\n"
        "    .section .multiboot\n"
        "    .align 4\n"
        "    .long 0x1BADB002          /* magic  */\n"
        "    .long 0x00000003          /* flags: 内存信息 */\n"
        "    .long -(0x1BADB002 + 3)   /* checksum */\n"
        "\n"
        "    .section .bss\n"
        "    .align 16\n"
        "stack_bottom:\n"
        "    .skip 16384\n"
        "stack_top:\n"
        "\n"
        "    .section .text.init\n"
        "    .globl _start\n"
        "_start:\n"
        "    mov  $stack_top, %esp\n"
        "    call kmain\n"
        "1:  cli\n"
        "    hlt\n"
        "    jmp 1b\n";
}

static void tpl_linker(const target_desc *t, char *buf, size_t n)
{
    snprintf(buf, n,
        "/* 链接脚本: 段布局与关键符号 */\n"
        "ENTRY(_start)\n"
        "MEMORY {\n"
        "    ram (rwx) : ORIGIN = %s, LENGTH = %s\n"
        "}\n"
        "SECTIONS {\n"
        "    .text : {\n"
        "        %s\n"
        "        KEEP(*(.text.init))\n"
        "        *(.text .text.*)\n"
        "    } > ram\n"
        "    .rodata : { *(.rodata .rodata.*) } > ram\n"
        "    .data : {\n"
        "        __global_pointer$ = . + 0x800;\n"
        "        *(.data .data.* .sdata .sdata.*)\n"
        "    } > ram\n"
        "    .bss : {\n"
        "        __bss_start = .;\n"
        "        *(.bss .bss.* .sbss .sbss.*) *(COMMON)\n"
        "        __bss_end = .;\n"
        "    } > ram\n"
        "    . = ALIGN(16);\n"
        "    __stack_top = ORIGIN(ram) + LENGTH(ram);\n"
        "}\n",
        t->ram_base, t->ram_len,
        strcmp(t->target, "i386") == 0 ? "KEEP(*(.multiboot))" : "");
}

static void tpl_kernel(const target_desc *t, const char *name, char *buf, size_t n)
{
    if (t->uart == UART_NS16550)
        snprintf(buf, n,
            "/* %s —— QEMU virt (RISC-V) 裸机内核骨架 */\n"
            "#include <stdint.h>\n\n"
            "#define UART0 0x10000000UL              /* NS16550 */\n"
            "static volatile uint8_t *const uart = (volatile uint8_t *)UART0;\n\n"
            "static void uart_putc(char c)\n{\n"
            "    while (!(uart[5] & 0x20)) { }      /* LSR: THR 空 */\n"
            "    uart[0] = (uint8_t)c;\n"
            "}\n\n"
            "void uart_puts(const char *s)\n{\n"
            "    while (*s)\n        uart_putc(*s++);\n"
            "}\n\n"
            "void kmain(void)\n{\n"
            "    uart_puts(\"\\n[%s] hello, bare-metal world!\\n\");\n"
            "    uart_puts(\"[%s] QEMU virt · NS16550 @ 0x10000000 · RAM @ 0x80000000\\n\");\n"
            "    for (;;)\n"
            "        __asm__ volatile(\"wfi\");\n"
            "}\n", name, name, name);
    else if (t->uart == UART_PL011)
        snprintf(buf, n,
            "/* %s —— QEMU virt (ARM) 裸机内核骨架 */\n"
            "#include <stdint.h>\n\n"
            "#define UART0 0x09000000UL              /* PL011 */\n"
            "static volatile uint32_t *const uart = (volatile uint32_t *)UART0;\n\n"
            "static void uart_putc(char c)\n{\n"
            "    while (uart[6] & (1u << 5)) { }    /* FR: 发送满 */\n"
            "    uart[0] = (uint32_t)(unsigned char)c;\n"
            "}\n\n"
            "void uart_puts(const char *s)\n{\n"
            "    while (*s)\n        uart_putc(*s++);\n"
            "}\n\n"
            "void kmain(void)\n{\n"
            "    uart_puts(\"\\n[%s] hello, bare-metal world!\\n\");\n"
            "    uart_puts(\"[%s] QEMU virt · PL011 @ 0x09000000 · RAM @ 0x40000000\\n\");\n"
            "    for (;;)\n"
            "        __asm__ volatile(\"wfe\");\n"
            "}\n", name, name, name);
    else
        snprintf(buf, n,
            "/* %s —— QEMU i386 (Multiboot) 裸机内核骨架 */\n"
            "#include <stdint.h>\n\n"
            "static inline void outb(uint16_t p, uint8_t v)\n{\n"
            "    __asm__ volatile(\"outb %%0, %%1\" : : \"a\"(v), \"Nd\"(p));\n"
            "}\n"
            "static inline uint8_t inb(uint16_t p)\n{\n"
            "    uint8_t v;\n"
            "    __asm__ volatile(\"inb %%1, %%0\" : \"a\"(v) : \"Nd\"(p));\n"
            "    return v;\n"
            "}\n\n"
            "#define COM1 0x3F8\n\n"
            "static void serial_init(void)\n{\n"
            "    outb(COM1 + 1, 0x00);\n"
            "    outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x01);\n"
            "    outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7);\n"
            "}\n\n"
            "static void uart_putc(char c)\n{\n"
            "    while (!(inb(COM1 + 5) & 0x20)) { }\n"
            "    outb(COM1, (uint8_t)c);\n"
            "}\n\n"
            "void uart_puts(const char *s)\n{\n"
            "    while (*s)\n        uart_putc(*s++);\n"
            "}\n\n"
            "void kmain(void)\n{\n"
            "    serial_init();\n"
            "    uart_puts(\"\\n[%s] hello, bare-metal world!\\n\");\n"
            "    uart_puts(\"[%s] QEMU i386 · COM1 0x3F8 · Multiboot @ 0x100000\\n\");\n"
            "    for (;;) {\n"
            "        __asm__ volatile(\"hlt\");\n"
            "    }\n"
            "}\n", name, name, name);
}

static void tpl_makefile(const target_desc *t, const char *name, char *buf, size_t n)
{
    char runline[512];
    if (t->machine[0])
        snprintf(runline, sizeof runline, "$(QEMU) -M %s %s -nographic -kernel %s.elf",
                 t->machine, t->qemu_extra, name);
    else
        snprintf(runline, sizeof runline, "$(QEMU) -kernel %s.elf -nographic", name);
    snprintf(buf, n,
        "CROSS ?= %s\n"
        "CC    = $(CROSS)gcc\n"
        "QEMU  = %s\n"
        "CFLAGS = -ffreestanding -nostdlib -nostartfiles -fno-builtin -fno-pie \\\n"
        "         -fno-stack-protector -O2 -Wall -Wextra -g %s\n"
        "\n"
        "all: %s.elf\n\n"
        "%s.elf: crt0.S linker.ld kernel.c\n"
        "\t$(CC) $(CFLAGS) -T linker.ld crt0.S kernel.c -o $@\n\n"
        "run: all\n"
        "\t%s\n\n"
        "clean:\n"
        "\trm -f %s.elf\n\n"
        ".PHONY: all run clean\n",
        t->cross, t->qemu, t->arch_flag, name, name, runline, name);
}

static int cmd_gen(int argc, char **argv)
{
    const char *target = NULL, *name = NULL, *dir = NULL;
    int force = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--target=", 9) == 0) target = argv[i] + 9;
        else if (strncmp(argv[i], "--name=", 7) == 0) name = argv[i] + 7;
        else if (strncmp(argv[i], "--dir=", 6) == 0) dir = argv[i] + 6;
        else if (arg_is(argv[i], "--force")) force = 1;
        else if (arg_is(argv[i], "--help") || arg_is(argv[i], "-h")) { port_usage(); return 0; }
        else { fprintf(stderr, "oskit port gen: 未知参数 %s\n", argv[i]); return 2; }
    }
    const target_desc *t = find_target(target ? target : "");
    if (!t) {
        fprintf(stderr, "oskit port gen: 未知 --target (可选:");
        for (size_t i = 0; i < NTGT; i++)
            fprintf(stderr, " %s", g_targets[i].target);
        fprintf(stderr, ")\n");
        return 2;
    }
    if (!name || !*name) {
        fprintf(stderr, "oskit port gen: 需要 --name=<工程名>\n");
        return 2;
    }
    char outdir[1024];
    if (dir)
        snprintf(outdir, sizeof outdir, "%s/%s", dir, name);
    else
        snprintf(outdir, sizeof outdir, "%s", name);
    struct stat st;
    if (stat(outdir, &st) == 0 && !force) {
        fprintf(stderr, "oskit port gen: %s 已存在 (--force 覆盖)\n", outdir);
        return 1;
    }
    if (dir && mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "oskit port gen: 无法创建 %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (mkdir(outdir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "oskit port gen: 无法创建 %s: %s\n", outdir, strerror(errno));
        return 1;
    }

    const char *crt0 =
        (t->uart == UART_PL011) ? (strcmp(t->target, "aarch64") == 0 ? tpl_crt0_aarch64() : tpl_crt0_armv7()) :
        (strcmp(t->target, "i386") == 0) ? tpl_crt0_i386() : tpl_crt0_riscv();
    char linker[4096], kernel[4096], makefile[4096], readme[4096], runsh[512];
    tpl_linker(t, linker, sizeof linker);
    tpl_kernel(t, name, kernel, sizeof kernel);
    tpl_makefile(t, name, makefile, sizeof makefile);
    snprintf(runsh, sizeof runsh,
             "#!/bin/sh\n# 自包含 QEMU 启动脚本(不依赖 oskit)\n"
             "exec %s %s %s -nographic -kernel %s.elf \"$@\"\n",
             t->qemu, t->machine, t->qemu_extra, name);
    snprintf(readme, sizeof readme,
        "# %s (%s 裸机骨架)\n\n"
        "由 `oskit port gen --target %s` 生成。\n\n"
        "## 构建\n\n    make            # 需要: %s\n\n"
        "## 运行\n\n    make run        # 或 ./run.sh (需 %s)\n\n"
        "## 内存映射(QEMU %s)\n\n- RAM: 0x%s (%s)\n- 串口: %s\n\n"
        "## 下一步\n\n1. 在 kernel.c 里加全局数组/函数观察 .data/.bss 布局;\n"
        "2. 在 linker.ld 调整段位置或改内存基址(实机移植从这里开始);\n"
        "3. 加中断向量表与定时器(RISC-V: mtvec/clint; ARM: vbar/GIC; x86: IDT/PIC)。\n",
        name, t->cn, t->target,
        t->cross[0] ? t->cross : "宿主 gcc -m32 (Ubuntu 需 gcc-multilib)",
        t->qemu, t->machine[0] ? t->machine : "i386",
        t->ram_base, t->ram_len,
        t->uart == UART_NS16550 ? "NS16550 @ 0x10000000" :
        t->uart == UART_PL011 ? "PL011 @ 0x09000000" : "COM1 @ 0x3F8");

    int rc = 0;
    rc |= write_file(outdir, "crt0.S", crt0);
    rc |= write_file(outdir, "linker.ld", linker);
    rc |= write_file(outdir, "kernel.c", kernel);
    rc |= write_file(outdir, "Makefile", makefile);
    rc |= write_file(outdir, "run.sh", runsh);
    rc |= write_file(outdir, "README.md", readme);
    if (rc != 0)
        return 1;
    char p[1100];
    snprintf(p, sizeof p, "%s/run.sh", outdir);
    chmod(p, 0755);
    printf("已生成 %s/ (%s · QEMU %s)\n", outdir, t->cn,
           t->machine[0] ? t->machine : "-kernel multiboot");
    printf("下一步: cd %s && make && make run  (工具链: %s%s)\n", outdir,
           t->cross[0] ? t->cross : "宿主 gcc",
           t->cross[0] ? "gcc" : " -m32 (需 gcc-multilib)");
    return 0;
}

/* ===================================================================== */
/* run —— 按目标机拼装并执行 QEMU                                          */
/* ===================================================================== */
static int cmd_run(int argc, char **argv)
{
    const char *target = NULL, *elf = NULL;
    int dry = 0, extra_at = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--target=", 9) == 0) target = argv[i] + 9;
        else if (strncmp(argv[i], "--elf=", 6) == 0) elf = argv[i] + 6;
        else if (arg_is(argv[i], "--dry")) dry = 1;
        else if (arg_is(argv[i], "--")) { extra_at = i + 1; break; }
        else { fprintf(stderr, "oskit port run: 未知参数 %s\n", argv[i]); return 2; }
    }
    const target_desc *t = find_target(target ? target : "");
    if (!t || !elf) {
        fprintf(stderr, "用法: oskit port run --target=<t> --elf=<文件> [--dry] [-- qemu参数]\n");
        return 2;
    }
    if (access(elf, R_OK) != 0) {
        fprintf(stderr, "oskit port run: 无法读取 %s: %s\n", elf, strerror(errno));
        return 1;
    }
    char *qargs[32];
    int n = 0;
    qargs[n++] = (char *)t->qemu;
    if (t->machine[0]) {
        qargs[n++] = (char *)"-M";
        qargs[n++] = (char *)t->machine;
    }
    if (t->qemu_extra[0]) {
        /* extra 可能是 "-bios none" 或 "-cpu cortex-a57" 两个词 */
        char *ex = strdup(t->qemu_extra);
        for (char *tok = strtok(ex, " "); tok && n < 28; tok = strtok(NULL, " "))
            qargs[n++] = tok;
    }
    qargs[n++] = (char *)"-nographic";
    qargs[n++] = (char *)"-kernel";
    qargs[n++] = (char *)elf;
    if (extra_at)
        for (int i = extra_at; i < argc && n < 31; i++)
            qargs[n++] = argv[i];
    qargs[n] = NULL;

    printf("QEMU: ");
    for (int i = 0; i < n; i++)
        printf("%s ", qargs[i]);
    putchar('\n');
    if (dry)
        return 0;
    execvp(qargs[0], qargs);
    fprintf(stderr, "oskit port run: 无法启动 %s (%s) — 先 oskit port check 检查环境\n",
            t->qemu, strerror(errno));
    return 1;
}

/* ===================================================================== */
/* image —— 裸磁盘镜像骨架                                                 */
/* ===================================================================== */
static int cmd_image(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    unsigned long mib = 16, offset = 512;
    int mbr = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--input=", 8) == 0) input = argv[i] + 8;
        else if (strncmp(argv[i], "--output=", 9) == 0) output = argv[i] + 9;
        else if (strncmp(argv[i], "--size=", 7) == 0) mib = strtoul(argv[i] + 7, NULL, 0);
        else if (strncmp(argv[i], "--offset=", 9) == 0) offset = strtoul(argv[i] + 9, NULL, 0);
        else if (arg_is(argv[i], "--mbr")) mbr = 1;
        else { fprintf(stderr, "oskit port image: 未知参数 %s\n", argv[i]); return 2; }
    }
    if (!input || !output) {
        fprintf(stderr, "用法: oskit port image --input=<内核> --output=<img> "
                        "[--size=MiB] [--offset=N] [--mbr]\n");
        return 2;
    }
    void *buf;
    size_t len;
    if (xread_file(input, &buf, &len) != 0) {
        fprintf(stderr, "oskit port image: 无法读取 %s: %s\n", input, strerror(errno));
        return 1;
    }
    FILE *f = fopen(output, "wb");
    if (!f) {
        fprintf(stderr, "oskit port image: 无法创建 %s: %s\n", output, strerror(errno));
        free(buf);
        return 1;
    }
    if (ftruncate(fileno(f), 0) != 0) { /* 清空 */ }
    size_t total = (size_t)mib << 20;
    uint8_t zero[4096];
    memset(zero, 0, sizeof zero);
    for (size_t w = 0; w < total; w += sizeof zero)
        fwrite(zero, 1, sizeof zero, f);
    if (mbr) {
        uint8_t sect[512];
        memset(sect, 0, sizeof sect);
        sect[0] = 0xF4; sect[1] = 0xEB; sect[2] = 0xFD; /* hlt; jmp $-1 占位 */
        sect[510] = 0x55;
        sect[511] = 0xAA;
        fseek(f, 0, SEEK_SET);
        fwrite(sect, 1, sizeof sect, f);
    }
    if (fseek(f, (long)offset, SEEK_SET) != 0 ||
        fwrite(buf, 1, len, f) != len) {
        fprintf(stderr, "oskit port image: 写入内核失败\n");
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);
    free(buf);
    printf("镜像 %s: %lu MiB%s, 内核 %zu 字节 @ 偏移 %lu%s\n",
           output, mib, mbr ? " (含 MBR 55AA 签名与 halt 占位引导扇区)" : "",
           len, offset, mbr ? "" : " (--mbr 可加 MBR 签名)");
    return 0;
}

/* ===================================================================== */
static void port_usage(void)
{
    puts("用法: oskit port <子命令> ...");
    puts("");
    puts("  check [--json]                      环境体检(交叉工具链/QEMU)");
    puts("  gen --target=<t> --name=<n> [--dir=<d>] [--force]");
    puts("                                      生成裸机工程骨架");
    puts("  run --target=<t> --elf=<f> [--dry] [-- qemu参数]");
    puts("                                      按目标机启动 QEMU");
    puts("  image --input=<f> --output=<i> [--size=MiB] [--offset=N] [--mbr]");
    puts("                                      生成裸磁盘镜像骨架");
    puts("");
    printf("target 可选:");
    for (size_t i = 0; i < NTGT; i++)
        printf(" %s(%s)", g_targets[i].target, g_targets[i].cn);
    putchar('\n');
}

int port_main(int argc, char **argv)
{
    if (argc < 2 || arg_is(argv[1], "--help") || arg_is(argv[1], "-h")) {
        port_usage();
        return argc < 2 ? 2 : 0;
    }
    if (strcmp(argv[1], "check") == 0)
        return cmd_check(argc - 1, argv + 1);
    if (strcmp(argv[1], "gen") == 0)
        return cmd_gen(argc - 1, argv + 1);
    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc - 1, argv + 1);
    if (strcmp(argv[1], "image") == 0)
        return cmd_image(argc - 1, argv + 1);
    fprintf(stderr, "oskit port: 未知子命令 %s\n", argv[1]);
    port_usage();
    return 2;
}
