/* analyze.h —— 指令级分析与安全检查 */
#ifndef RVLAB_ANALYZE_H
#define RVLAB_ANALYZE_H

#include <stdio.h>
#include "rv.h"
#include "elf.h"

/* 从 ELF 符号表提取已定义函数符号(升序), 供 rv_set_syms 做热点归因 */
int  analyze_collect_syms(elf_file *ef, rv_sym **out, size_t *n);

/* 运行画像: 指令组合直方图 / 分支命中率 / 访存计数 / 热点符号 Top5 */
void analyze_report(rv_vm *v, FILE *out, int json);

/* 加固检查(NX/PIE/RELRO/Canary/FORTIFY + 危险符号), 规则源自 ../oskit vuln */
void harden_report(const void *elfbuf, size_t len, const char *path, FILE *out, int json);

#endif
