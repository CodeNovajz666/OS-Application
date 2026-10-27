/* skeleton.c —— 裸机工程骨架生成（riscv32/riscv64，模板源自 ../oskit/src/port.c）*/
#include "skeleton.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static int wfile(const char *dir, const char *name, const char *content)
{
    char p[2048];
    snprintf(p, sizeof p, "%.1000s/%.1000s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f)
        return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

int skeleton_gen(const char *target, const char *name, const char *dir)
{
    if (!target || !name || !dir)
        return -1;
    int is64 = strcmp(target, "riscv64") == 0;
    if (!is64 && strcmp(target, "riscv32") != 0)
        return -1;
    const char *qemu = is64 ? "qemu-system-riscv64" : "qemu-system-riscv32";
    const char *cross = is64 ? "riscv64-unknown-elf-" : "riscv32-unknown-elf-";
    const char *march = is64 ? "rv64im" : "rv32im";

    char outdir[1024];
    snprintf(outdir, sizeof outdir, "%s/%s", dir, name);
    struct stat st;
    if (stat(outdir, &st) == 0)
        return -2;
    if (stat(dir, &st) != 0 && mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -3;
    if (mkdir(outdir, 0755) != 0)
        return -3;

    static const char crt0[] =
        "/* 启动代码: 设全局指针与栈, 进入内核入口 (来自 oskit port 模板) */\n"
        "    .section .text.init\n"
        "    .globl _start\n"
        "_start:\n"
        "    la   gp, __global_pointer$\n"
        "    la   sp, __stack_top\n"
        "    call kmain\n"
        "1:  wfi\n"
        "    j    1b\n";

    char linker[2048];
    snprintf(linker, sizeof linker,
        "ENTRY(_start)\n"
        "MEMORY { ram (rwx) : ORIGIN = 0x80000000, LENGTH = 128M }\n"
        "SECTIONS {\n"
        "    .text : { KEEP(*(.text.init)) *(.text .text.*) } > ram\n"
        "    .rodata : { *(.rodata .rodata.*) } > ram\n"
        "    .data : {\n"
        "        __global_pointer$ = . + 0x800;\n"
        "        *(.data .data.* .sdata .sdata.*)\n"
        "    } > ram\n"
        "    .bss : {\n"
        "        __bss_start = .; *(.bss .bss.* .sbss .sbss.*) *(COMMON) __bss_end = .;\n"
        "    } > ram\n"
        "    . = ALIGN(16);\n"
        "    __stack_top = ORIGIN(ram) + LENGTH(ram);\n"
        "}\n");

    char kernel[2048];
    snprintf(kernel, sizeof kernel,
        "/* %s —— QEMU virt (%s) 裸机内核骨架, 由 RVLab 生成 */\n"
        "#include <stdint.h>\n\n"
        "#define UART0 0x10000000UL\n"
        "static volatile uint8_t *const uart = (volatile uint8_t *)UART0;\n\n"
        "static void uart_putc(char c)\n{\n"
        "    while (!(uart[5] & 0x20)) { }\n"
        "    uart[0] = (uint8_t)c;\n"
        "}\n"
        "void uart_puts(const char *s)\n{\n"
        "    while (*s) uart_putc(*s++);\n"
        "}\n"
        "void kmain(void)\n{\n"
        "    uart_puts(\"\\n[%s] hello, bare-metal world!\\n\");\n"
        "    uart_puts(\"在 RVLab 中: build 后 run, 或用 oskit emu 运行本工程产物\\n\");\n"
        "    for (;;) __asm__ volatile(\"wfi\");\n"
        "}\n", name, target, name);

    char makefile[2048];
    snprintf(makefile, sizeof makefile,
        "CROSS ?= %s\n"
        "CC    = $(CROSS)gcc\n"
        "QEMU  = %s\n"
        "CFLAGS = -march=%s -mabi=%s -ffreestanding -nostdlib -nostartfiles \\\n"
        "         -fno-builtin -fno-pie -O2 -Wall -Wextra -g\n"
        "\n"
        "all: %s.elf\n\n"
        "%s.elf: crt0.S linker.ld kernel.c\n"
        "\t$(CC) $(CFLAGS) -T linker.ld crt0.S kernel.c -o $@\n\n"
        "run: all\n"
        "\t$(QEMU) -M virt -bios none -nographic -kernel %s.elf\n\n"
        "clean:\n\trm -f %s.elf\n\n.PHONY: all run clean\n",
        cross, qemu, march, is64 ? "lp64" : "ilp32", name, name, name, name);

    char runsh[256];
    snprintf(runsh, sizeof runsh,
        "#!/bin/sh\nexec %s -M virt -bios none -nographic -kernel %s.elf \"$@\"\n",
        qemu, name);

    char readme[2048];
    snprintf(readme, sizeof readme,
        "# %s (%s 裸机骨架)\n\n由 RVLab `skeleton` 生成(模板源自 oskit port)。\n\n"
        "    make        # 需 %s\n    make run    # 需 %s\n\n"
        "内存映射: RAM 0x80000000 · NS16550 串口 0x10000000 (与 RVLab/oskit emu 一致)。\n",
        name, target, cross, qemu);

    int rc = 0;
    rc |= wfile(outdir, "crt0.S", crt0);
    rc |= wfile(outdir, "linker.ld", linker);
    rc |= wfile(outdir, "kernel.c", kernel);
    rc |= wfile(outdir, "Makefile", makefile);
    rc |= wfile(outdir, "run.sh", runsh);
    rc |= wfile(outdir, "README.md", readme);
    if (rc)
        return -3;
    char p[1100];
    snprintf(p, sizeof p, "%s/run.sh", outdir);
    chmod(p, 0755);
    return 0;
}
