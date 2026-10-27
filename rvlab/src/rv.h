/* rv.h —— RV32I+M 虚拟机（库化）
 * 执行内核移植自 ../oskit/src/emu.c（已实测），改造点：
 * 句柄化(无全局状态) / 断点表 / 指令分类统计 / UART·跟踪回调 / 错误返回码 */
#ifndef RVLAB_RV_H
#define RVLAB_RV_H

#include <stddef.h>
#include <stdint.h>

/* 内存映射（与 QEMU virt 子集一致，详见 docs/rvlab.md）*/
#define RV_RAM_BASE   0x80000000u
#define RV_UART_BASE  0x10000000u
#define RV_MTIME_ADDR 0x0200BFF8u
#define RV_EXIT_BASE  0x90000000u

typedef void (*rv_uart_fn)(void *ud, uint8_t byte);
typedef void (*rv_trace_fn)(void *ud, uint32_t pc, uint32_t inst);

/* 热点归因用的符号（由调用方从 elf 提供, 按 addr 升序）*/
typedef struct {
    uint64_t addr, size;
    char     name[64];
} rv_sym;

/* 指令类别（统计直方图的桶）*/
enum {
    RV_OP_LUI, RV_OP_AUIPC, RV_OP_JAL, RV_OP_JALR, RV_OP_BRANCH,
    RV_OP_LOAD, RV_OP_STORE, RV_OP_OPIMM, RV_OP_OP, RV_OP_FENCE,
    RV_OP_SYSTEM, RV_OP_MULDIV, RV_OP_CLASS_N
};

typedef struct {
    uint32_t x[32], pc;
    uint64_t instret;
    uint8_t *ram;
    size_t   ram_size;
    int      exited, exit_code;

    uint64_t bp[64];
    int      n_bp;
    uint64_t hit_bp;        /* 最近一次断点停下的地址(未执行) */

    /* 统计 */
    uint64_t op_count[RV_OP_CLASS_N];
    uint64_t br_total, br_taken, load_cnt, store_cnt;
    uint64_t *sym_count;    /* 每符号执行条数, 与 syms 一一对应(可 NULL) */

    rv_sym  *syms;          /* 升序符号表(可 NULL) */
    size_t   n_syms;

    rv_uart_fn  uart_fn;  void *uart_ud;
    rv_trace_fn trace_fn; void *trace_ud;   /* 每条执行后回调 */

    char err[160];          /* 返回 RV_ERR 时有效 */
} rv_vm;

#define RV_OK        0   /* rv_step: 已执行一条 */
#define RV_EXIT      1   /* 程序退出(exit_code 有效) */
#define RV_BREAK     2   /* 断点停(pc 指向断点, 未执行) */
#define RV_ERR       3   /* vm->err 有描述 */
#define RV_MAXSTEPS  4   /* 达到步数上限 */

rv_vm *rv_create(size_t ram_mib);
void   rv_free(rv_vm *v);
void   rv_reset(rv_vm *v, uint32_t entry);           /* 寄存器/统计清零, 程序保留 */

int rv_load_elf(rv_vm *v, const void *buf, size_t len);          /* 0 成功 */
int rv_load_flat(rv_vm *v, const void *buf, size_t len, uint32_t base);
void rv_set_syms(rv_vm *v, const rv_sym *tab, size_t n);         /* 拷贝并分配计数 */

int  rv_bp_add(rv_vm *v, uint64_t addr);
void rv_bp_clear(rv_vm *v);

int rv_step(rv_vm *v);                       /* RV_OK/RV_EXIT/RV_BREAK/RV_ERR */
int rv_run(rv_vm *v, uint64_t max_steps);    /* RV_EXIT/RV_BREAK/RV_ERR/RV_MAXSTEPS */

/* 轻量反汇编(与 oskit emu 同源, 供面板/跟踪/gdbstub 使用) */
void rv_disasm(uint32_t inst, char *out, size_t n);
/* 虚拟机内读取内存字(不触发 MMIO 统计), 供面板/gdbstub: 失败返回 0 */
int  rv_peek(rv_vm *v, uint32_t addr, uint8_t *bytes, size_t n);
int  rv_poke(rv_vm *v, uint32_t addr, const uint8_t *bytes, size_t n);

const char *rv_op_class_name(int cls);
const char *rv_reg_name(int r);

#endif
