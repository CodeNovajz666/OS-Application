/* emu.c —— 系统模拟器：RISC-V RV32I + M 扩展指令集模拟（用户态解释执行）
 * 设计见 docs/05-emu.md。内存映射对齐 QEMU virt 机子集：
 *   RAM 0x80000000 · UART(NS16550) 0x10000000 · CLINT mtime 0x0200BFF8 · Exit 0x90000000 */
#include "common.h"
#include "elf.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RAM_BASE   0x80000000u
#define UART_BASE  0x10000000u
#define MTIME_ADDR 0x0200BFF8u
#define EXIT_BASE  0x90000000u

#define EM_RISCV   243

/* ===================================================================== */
/* 内置演示程序（手工编码, 每条指令附编码注释, --trace 可互证）             */
/* ===================================================================== */
/* demo1: 打印 "EMU OK\n" 后经 Exit 设备退出(码 42) */
static const uint32_t g_demo1[] = {
    0x100002B7,  /* lui   t0, 0x10000      ; t0 = 0x10000000 (UART)          */
    0x04500313,  /* addi  t1, zero, 69     ; 'E'                              */
    0x00628023,  /* sb    t1, 0(t0)                                          */
    0x04D00313,  /* addi  t1, zero, 77     ; 'M'                              */
    0x00628023,  /* sb    t1, 0(t0)                                          */
    0x05500313,  /* addi  t1, zero, 85     ; 'U'                              */
    0x00628023,  /* sb    t1, 0(t0)                                          */
    0x02000313,  /* addi  t1, zero, 32     ; ' '                              */
    0x00628023,  /* sb    t1, 0(t0)                                          */
    0x04F00313,  /* addi  t1, zero, 79     ; 'O'                              */
    0x00628023,  /* sb    t1, 0(t0)                                          */
    0x04B00313,  /* addi  t1, zero, 75     ; 'K'                              */
    0x00628023,  /* sb    t1, 0(t0)                                          */
    0x00A00313,  /* addi  t1, zero, 10     ; '\n'                             */
    0x00628023,  /* sb    t1, 0(t0)                                          */
    0x900003B7,  /* lui   t2, 0x90000      ; t2 = 0x90000000 (Exit 设备)     */
    0x02A00E13,  /* addi  t3, zero, 42                                       */
    0x01C3A023,  /* sw    t3, 0(t2)        ; exit(42)                         */
};

/* demo2: 打印 "fib(10)=" 后迭代计算斐波那契, divu/remu 转十进制输出 "55", 退出码 55 */
static const uint32_t g_demo2[] = {
    0x80800137,  /* [0]  lui   sp, 0x80800      ; sp = 0x80800000 (RAM 顶)   */
    0x100002B7,  /* [1]  lui   t0, 0x10000      ; UART                       */
    0x06600F13,  /* [2]  addi  t5, zero, 102    ; 'f'                        */
    0x01E28023,  /* [3]  sb    t5, 0(t0)                                     */
    0x06900F13,  /* [4]  addi  t5, zero, 105    ; 'i'                        */
    0x01E28023,  /* [5]  sb    t5, 0(t0)                                     */
    0x06200F13,  /* [6]  addi  t5, zero, 98     ; 'b'                        */
    0x01E28023,  /* [7]  sb    t5, 0(t0)                                     */
    0x02800F13,  /* [8]  addi  t5, zero, 40     ; '('                        */
    0x01E28023,  /* [9]  sb    t5, 0(t0)                                     */
    0x03100F13,  /* [10] addi  t5, zero, 49     ; '1'                        */
    0x01E28023,  /* [11] sb    t5, 0(t0)                                     */
    0x03000F13,  /* [12] addi  t5, zero, 48     ; '0'                        */
    0x01E28023,  /* [13] sb    t5, 0(t0)                                     */
    0x02900F13,  /* [14] addi  t5, zero, 41     ; ')'                        */
    0x01E28023,  /* [15] sb    t5, 0(t0)                                     */
    0x03D00F13,  /* [16] addi  t5, zero, 61     ; '='                        */
    0x01E28023,  /* [17] sb    t5, 0(t0)                                     */
    0x00000513,  /* [18] addi  a0, zero, 0      ; a = 0                      */
    0x00100593,  /* [19] addi  a1, zero, 1      ; b = 1                      */
    0x00A00313,  /* [20] addi  t1, zero, 10     ; 计数                       */
    0x00B503B3,  /* [21] loop: add  t2, a0, a1                                */
    0x00058513,  /* [22] addi  a0, a1, 0                                       */
    0x00038593,  /* [23] addi  a1, t2, 0                                       */
    0xFFF30313,  /* [24] addi  t1, t1, -1                                      */
    0xFE0318E3,  /* [25] bne   t1, zero, loop      ; 偏移 -16                */
    0x00050613,  /* [26] cvt:  addi a2, a0, 0       ; n = fib                */
    0x00000313,  /* [27] addi  t1, zero, 0         ; 位数                    */
    0x00A00E93,  /* [28] cvt_loop: addi t4, zero, 10                          */
    0x03D67F33,  /* [29] remu  t5, a2, t4          ; digit = n % 10          */
    0x03D65633,  /* [30] divu  a2, a2, t4          ; n /= 10                 */
    0xFFC10113,  /* [31] addi  sp, sp, -4                                     */
    0x01E12023,  /* [32] sw    t5, 0(sp)            ; 入栈                    */
    0x00130313,  /* [33] addi  t1, t1, 1                                       */
    0xFE0614E3,  /* [34] bne   a2, zero, cvt_loop   ; 偏移 -24                */
    0x00012F03,  /* [35] emit: lw   t5, 0(sp)          ; 出栈                 */
    0x00410113,  /* [36] addi  sp, sp, 4                                        */
    0x030F0F13,  /* [37] addi  t5, t5, 48            ; + '0'                 */
    0x01E28023,  /* [38] sb    t5, 0(t0)                                       */
    0xFFF30313,  /* [39] addi  t1, t1, -1                                      */
    0xFE0316E3,  /* [40] bne   t1, zero, emit        ; 偏移 -20               */
    0x00A00F13,  /* [41] addi  t5, zero, 10           ; '\n'                 */
    0x01E28023,  /* [42] sb    t5, 0(t0)                                       */
    0x90000FB7,  /* [43] lui   t6, 0x90000           ; Exit 设备             */
    0x00AFA023,  /* [44] sw    a0, 0(t6)             ; exit(fib=55)          */
};

/* ===================================================================== */
/* 虚拟机                                                                 */
/* ===================================================================== */
typedef struct {
    uint32_t x[32], pc;
    uint64_t instret;
    uint8_t *ram;
    size_t   ram_size;
    int      exited, exit_code;
    int      trace;
    uint64_t trace_after, max;
} vm_t;

static const char *RN[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static void set_reg(vm_t *v, unsigned rd, uint32_t val)
{
    if (rd) v->x[rd] = val; /* x0 恒零 */
}

static uint32_t sext(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

enum { A_RAM, A_UART, A_MTIME, A_EXIT, A_FAULT };

static int addr_kind(const vm_t *v, uint32_t a)
{
    if (a >= RAM_BASE && (uint64_t)a < (uint64_t)RAM_BASE + v->ram_size) return A_RAM;
    if (a >= UART_BASE && a < UART_BASE + 0x100) return A_UART;
    if (a >= MTIME_ADDR && a < MTIME_ADDR + 8)   return A_MTIME;
    if (a >= EXIT_BASE && a < EXIT_BASE + 8)     return A_EXIT;
    return A_FAULT;
}

static uint8_t vm_ld8(vm_t *v, uint32_t a, int *ok)
{
    switch (addr_kind(v, a)) {
    case A_RAM:   *ok = 1; return v->ram[a - RAM_BASE];
    case A_UART:  *ok = 1; return (a & 0xff) == 0x05 ? 0x20 : 0; /* LSR: THR 空 */
    case A_MTIME: { /* mtime = 已执行指令数(简化模型) */
        *ok = 1;
        uint64_t t = v->instret;
        int off = (int)(a - MTIME_ADDR);
        return (uint8_t)(t >> (off * 8));
    }
    default: *ok = 0; return 0;
    }
}

static void vm_st8(vm_t *v, uint32_t a, uint8_t val)
{
    switch (addr_kind(v, a)) {
    case A_RAM:
        v->ram[a - RAM_BASE] = val;
        break;
    case A_UART:
        if ((a & 0xff) == 0x00) { /* THR → stdout */
            putchar(val);
            fflush(stdout);
        }
        break;
    case A_EXIT:
        if ((a & 7) == 0) {
            v->exited = 1;
            v->exit_code = val & 0xff;
        }
        break;
    default:
        break; /* 写未映射地址: 忽略(宽松) */
    }
}

static uint32_t vm_ld32(vm_t *v, uint32_t a, int *ok)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        int o = 1;
        uint8_t b = vm_ld8(v, a + (uint32_t)i, &o);
        if (!o) { *ok = 0; return 0; }
        r |= (uint32_t)b << (8 * i);
    }
    *ok = 1;
    return r;
}

static void vm_st32(vm_t *v, uint32_t a, uint32_t val)
{
    for (int i = 0; i < 4; i++)
        vm_st8(v, a + (uint32_t)i, (uint8_t)(val >> (8 * i)));
}

static uint32_t vm_fetch(vm_t *v, int *ok)
{
    if ((v->pc & 3) || addr_kind(v, v->pc) != A_RAM) {
        *ok = 0;
        return 0;
    }
    uint32_t w;
    memcpy(&w, v->ram + (v->pc - RAM_BASE), 4);
    *ok = 1;
    return w;
}

/* ---------- 轻量反汇编(供 --trace) ---------- */
static void disasm(uint32_t in, char *buf, size_t n)
{
    unsigned op = in & 0x7f;
    unsigned rd = (in >> 7) & 31, f3 = (in >> 12) & 7;
    unsigned rs1 = (in >> 15) & 31, rs2 = (in >> 20) & 31, f7 = in >> 25;
    int32_t immI = (int32_t)in >> 20;
    int32_t immS = ((int32_t)(in & 0xfe000000) >> 20) | (int32_t)((in >> 7) & 0x1f);
    int32_t immB = (int32_t)sext(((in >> 31) << 12) | (((in >> 7) & 1) << 11) |
                                 (((in >> 25) & 0x3f) << 5) | (((in >> 8) & 0xf) << 1), 13);
    uint32_t immU = in & 0xfffff000;
    int32_t immJ = (int32_t)sext(((in >> 31) << 20) | (((in >> 12) & 0xff) << 12) |
                                 (((in >> 20) & 1) << 11) | (((in >> 21) & 0x3ff) << 1), 21);
    switch (op) {
    case 0x37: snprintf(buf, n, "lui   %s, 0x%x", RN[rd], immU >> 12); break;
    case 0x17: snprintf(buf, n, "auipc %s, 0x%x", RN[rd], immU >> 12); break;
    case 0x6f: snprintf(buf, n, "jal   %s, %+d", RN[rd], immJ); break;
    case 0x67: snprintf(buf, n, "jalr  %s, %d(%s)", RN[rd], immI, RN[rs1]); break;
    case 0x63: {
        const char *b[] = { "beq", "bne", "?", "?", "blt", "bge", "bltu", "bgeu" };
        snprintf(buf, n, "%-5s %s, %s, %+d", b[f3], RN[rs1], RN[rs2], immB);
        break;
    }
    case 0x03: {
        const char *l[] = { "lb", "lh", "lw", "?", "lbu", "lhu", "?", "?" };
        snprintf(buf, n, "%-5s %s, %d(%s)", l[f3], RN[rd], immI, RN[rs1]);
        break;
    }
    case 0x23: {
        const char *s[] = { "sb", "sh", "sw", "?", "?", "?", "?", "?" };
        snprintf(buf, n, "%-5s %s, %d(%s)", s[f3], RN[rs2], immS, RN[rs1]);
        break;
    }
    case 0x13: {
        const char *i[] = { "addi", "slli", "slti", "sltiu", "xori", "srli/srai", "ori", "andi" };
        snprintf(buf, n, "%-5s %s, %s, %d", i[f3], RN[rd], RN[rs1], immI);
        break;
    }
    case 0x33: {
        const char *r[] = { "add/sub", "sll", "slt", "sltu", "xor", "srl/sra", "or", "and" };
        const char *m[] = { "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu" };
        snprintf(buf, n, "%-5s %s, %s, %s", f7 ? m[f3] : r[f3], RN[rd], RN[rs1], RN[rs2]);
        break;
    }
    case 0x0f: snprintf(buf, n, "fence"); break;
    case 0x73:
        if (in == 0x00000073) snprintf(buf, n, "ecall");
        else if (in == 0x00100073) snprintf(buf, n, "ebreak");
        else snprintf(buf, n, "csr   0x%x", in >> 20);
        break;
    default: snprintf(buf, n, ".word 0x%08x", in); break;
    }
}

/* ---------- CSR 读(计数器) ---------- */
static uint64_t csr_read(vm_t *v, uint32_t addr)
{
    switch (addr) {
    case 0xc00: case 0xc01: case 0xc02: return v->instret;
    case 0xc80: case 0xc81: case 0xc82: return 0;
    default: return 0; /* 未实现 CSR 读 0(文档已声明) */
    }
}

/* ===================================================================== */
/* 执行一条指令                                                           */
/* ===================================================================== */
static void vm_step(vm_t *v)
{
    int ok;
    uint32_t in = vm_fetch(v, &ok);
    if (!ok)
        die("模拟器: 取指失败 pc=0x%08x (未映射或未对齐)", v->pc);
    if (v->trace && v->instret >= v->trace_after) {
        char d[96];
        disasm(in, d, sizeof d);
        printf("%08x: %08x  %s\n", v->pc, in, d);
    }

    unsigned op = in & 0x7f;
    unsigned rd = (in >> 7) & 31, f3 = (in >> 12) & 7;
    unsigned rs1 = (in >> 15) & 31, rs2 = (in >> 20) & 31, f7 = in >> 25;
    uint32_t a = v->x[rs1], b = v->x[rs2];
    int32_t immI = (int32_t)in >> 20;
    int32_t immS = (int32_t)(in & 0xfe000000) >> 20 | (int32_t)((in >> 7) & 0x1f);
    int32_t immB = (int32_t)sext(((in >> 31) << 12) | (((in >> 7) & 1) << 11) |
                                 (((in >> 25) & 0x3f) << 5) | (((in >> 8) & 0xf) << 1), 13);
    uint32_t immU = in & 0xfffff000u;
    int32_t immJ = (int32_t)sext(((in >> 31) << 20) | (((in >> 12) & 0xff) << 12) |
                                 (((in >> 20) & 1) << 11) | (((in >> 21) & 0x3ff) << 1), 21);
    uint32_t next = v->pc + 4;

    switch (op) {
    case 0x37: set_reg(v, rd, immU); break;                       /* LUI */
    case 0x17: set_reg(v, rd, v->pc + immU); break;               /* AUIPC */
    case 0x6f: set_reg(v, rd, v->pc + 4); next = v->pc + (uint32_t)immJ; break; /* JAL */
    case 0x67: {                                                  /* JALR */
        uint32_t t = v->pc + 4;
        next = (a + (uint32_t)immI) & ~1u;
        set_reg(v, rd, t); /* 先写 link 再跳转, 规避 rd==rs1 */
        break;
    }
    case 0x63: {                                                  /* 分支 */
        int take;
        switch (f3) {
        case 0: take = a == b; break;
        case 1: take = a != b; break;
        case 4: take = (int32_t)a <  (int32_t)b; break;
        case 5: take = (int32_t)a >= (int32_t)b; break;
        case 6: take = a <  b; break;
        case 7: take = a >= b; break;
        default: die("模拟器: 非法分支 f3=%u pc=0x%08x", f3, v->pc);
        }
        if (take) next = v->pc + (uint32_t)immB;
        break;
    }
    case 0x03: {                                                  /* LOAD */
        uint32_t addr = a + (uint32_t)immI;
        int o = 1;
        uint32_t val = 0;
        switch (f3) {
        case 0: val = (uint32_t)(int32_t)(int8_t)vm_ld8(v, addr, &o); break;
        case 1: {
            uint8_t lo = vm_ld8(v, addr, &o), hi = o ? vm_ld8(v, addr + 1, &o) : 0;
            val = (uint32_t)(int32_t)(int16_t)(lo | (hi << 8));
            break;
        }
        case 2: val = vm_ld32(v, addr, &o); break;
        case 4: val = vm_ld8(v, addr, &o); break;
        case 5: {
            uint8_t lo = vm_ld8(v, addr, &o), hi = o ? vm_ld8(v, addr + 1, &o) : 0;
            val = lo | ((uint32_t)hi << 8);
            break;
        }
        default: die("模拟器: 非法加载 f3=%u pc=0x%08x", f3, v->pc);
        }
        if (!o) die("模拟器: 加载未映射地址 0x%08x pc=0x%08x", addr, v->pc);
        set_reg(v, rd, val);
        break;
    }
    case 0x23: {                                                  /* STORE */
        uint32_t addr = a + (uint32_t)immS;
        switch (f3) {
        case 0: vm_st8(v, addr, (uint8_t)b); break;
        case 1:
            vm_st8(v, addr, (uint8_t)b);
            vm_st8(v, addr + 1, (uint8_t)(b >> 8));
            break;
        case 2: vm_st32(v, addr, b); break;
        default: die("模拟器: 非法存储 f3=%u pc=0x%08x", f3, v->pc);
        }
        break;
    }
    case 0x13: {                                                  /* OP-IMM */
        uint32_t imm = (uint32_t)immI;
        uint32_t r = 0;
        switch (f3) {
        case 0: r = a + imm; break;
        case 1: r = a << (imm & 31); break;
        case 2: r = ((int32_t)a < (int32_t)imm) ? 1 : 0; break;
        case 3: r = (a < imm) ? 1 : 0; break;
        case 4: r = a ^ imm; break;
        case 5:
            if (f7 == 0x00) r = a >> (imm & 31);
            else if (f7 == 0x20) r = (uint32_t)((int32_t)a >> (imm & 31));
            else die("模拟器: 非法 srai/srli pc=0x%08x", v->pc);
            break;
        case 6: r = a | imm; break;
        case 7: r = a & imm; break;
        }
        set_reg(v, rd, r);
        break;
    }
    case 0x33: {                                                  /* OP */
        uint32_t r = 0;
        if (f7 == 1) {                                            /* M 扩展 */
            int32_t sa = (int32_t)a, sb = (int32_t)b;
            switch (f3) {
            case 0: r = a * b; break;
            case 1: r = (uint32_t)((int64_t)sa * (int64_t)sb >> 32); break;
            case 2: r = (uint32_t)((int64_t)sa * (uint64_t)b >> 32); break;
            case 3: r = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
            case 4: r = b == 0 ? 0xffffffffu : (sa == INT32_MIN && sb == -1) ?
                        (uint32_t)INT32_MIN : (uint32_t)(sa / sb); break;
            case 5: r = b == 0 ? 0xffffffffu : a / b; break;
            case 6: r = b == 0 ? (uint32_t)sa : (sa == INT32_MIN && sb == -1) ? 0 :
                        (uint32_t)(sa % sb); break;
            case 7: r = b == 0 ? a : a % b; break;
            }
        } else if (f7 == 0 || f7 == 0x20) {
            switch (f3) {
            case 0: r = f7 ? a - b : a + b; break;
            case 1: r = a << (b & 31); break;
            case 2: r = ((int32_t)a < (int32_t)b) ? 1 : 0; break;
            case 3: r = (a < b) ? 1 : 0; break;
            case 4: r = a ^ b; break;
            case 5: r = f7 ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
            case 6: r = a | b; break;
            case 7: r = a & b; break;
            }
        } else {
            die("模拟器: 非法 funct7=%u pc=0x%08x", f7, v->pc);
        }
        set_reg(v, rd, r);
        break;
    }
    case 0x0f: break;                                             /* FENCE → nop */
    case 0x73: {                                                  /* SYSTEM */
        if (f3 == 0) {
            if (in == 0x00000073) {                               /* ECALL */
                uint32_t nr = v->x[17], a0 = v->x[10];
                if (nr == 93) {                                   /* exit */
                    v->exited = 1;
                    v->exit_code = (int)(a0 & 0xff);
                } else if (nr == 64) {                            /* write(fd,buf,len) */
                    uint32_t bufaddr = v->x[11], len = v->x[12];
                    int fd = (int)a0;
                    if (addr_kind(v, bufaddr) != A_RAM ||
                        (uint64_t)bufaddr + len > (uint64_t)RAM_BASE + v->ram_size)
                        die("模拟器: ecall write 参数越界 0x%08x+%u", bufaddr, len);
                    ssize_t w = write(fd == 2 ? 2 : 1,
                                      v->ram + (bufaddr - RAM_BASE), len);
                    set_reg(v, 10, (uint32_t)w);
                } else {
                    die("模拟器: 未实现 ecall a7=%u pc=0x%08x (支持 93=exit, 64=write)",
                        nr, v->pc);
                }
            } else if (in == 0x00100073) {                        /* EBREAK */
                printf("\n[ebreak] pc=0x%08x\n", v->pc);
                v->exited = 1;
                v->exit_code = 1;
            } else {
                die("模拟器: 非法 SYSTEM 指令 0x%08x pc=0x%08x", in, v->pc);
            }
        } else {                                                  /* CSR */
            uint32_t addr = in >> 20;
            uint64_t old = csr_read(v, addr);
            uint32_t src = (f3 >= 5) ? rs1 : a;
            switch (f3) {
            case 1: csr_read(v, addr); (void)src; break;          /* CSRRW(计数器只读) */
            case 2: case 3: break;                                /* CSRRS/RC */
            case 5: case 6: case 7: break;                        /* 立即数版 */
            default: die("模拟器: 非法 CSR f3=%u", f3);
            }
            set_reg(v, rd, (uint32_t)old);
        }
        break;
    }
    default:
        die("模拟器: 非法指令 0x%08x pc=0x%08x (指令 0x%x)", in, v->pc, op);
    }

    v->x[0] = 0;
    v->pc = next;
    v->instret++;
}

/* ===================================================================== */
/* 装载                                                                   */
/* ===================================================================== */
static void load_flat(vm_t *v, const void *buf, size_t len, uint32_t base)
{
    if (base != RAM_BASE || len > v->ram_size)
        die("平坦二进制不落入 RAM: base=0x%08x len=%zu (RAM %zu @0x%08x)",
            base, len, v->ram_size, (size_t)RAM_BASE);
    memcpy(v->ram, buf, len);
    v->pc = base;
}

static void load_elf(vm_t *v, const void *buf, size_t len)
{
    elf_file *ef = elf_open_buf(buf, len);
    if (!ef)
        die("不是合法的 ELF(仅小端 ELF32/ELF64)");
    if (elf_is64(ef))
        die("是 ELF64 程序, 本模拟器执行 RV32 (用 -march=rv32... 重新编译)");
    if (elf_machine(ef) != EM_RISCV)
        die("e_machine=%u 不是 RISC-V (243)", elf_machine(ef));
    size_t nph = elf_phdr_count(ef);
    for (size_t i = 0; i < nph; i++) {
        elf_phdr ph;
        elf_phdr_get(ef, i, &ph);
        if (ph.type != 1) /* PT_LOAD */
            continue;
        if (ph.vaddr < RAM_BASE ||
            (uint64_t)ph.vaddr + ph.memsz > (uint64_t)RAM_BASE + v->ram_size)
            die("PT_LOAD 段 [0x%llx,+0x%llx) 超出 RAM 范围", 
                (unsigned long long)ph.vaddr, (unsigned long long)ph.memsz);
        memcpy(v->ram + (ph.vaddr - RAM_BASE),
               (const uint8_t *)buf + ph.offset, (size_t)ph.filesz);
        /* memsz>filesz 的 .bss 部分本就为 0(calloc) */
    }
    if (elf_entry(ef) < RAM_BASE || elf_entry(ef) - RAM_BASE >= v->ram_size)
        die("入口 0x%llx 不在 RAM 内", (unsigned long long)elf_entry(ef));
    v->pc = (uint32_t)elf_entry(ef);
    elf_close(ef);
}

/* ===================================================================== */
/* 主流程                                                                 */
/* ===================================================================== */
static void emu_usage(void)
{
    puts("用法: oskit emu <程序.elf> [选项]   或   oskit emu --demo / --demo2");
    puts("");
    puts("内存映射: RAM 0x80000000 (默认 8MiB) · UART 0x10000000 · Exit 0x90000000");
    puts("  --binary          按平坦二进制装入(默认 ELF32)");
    puts("  --base ADDR       平坦模式装载地址(默认 0x80000000)");
    puts("  --ram MiB         RAM 大小(默认 8)");
    puts("  --trace           逐指令跟踪打印");
    puts("  --trace-after N   前 N 条不打印");
    puts("  --max N           指令上限(默认 5000 万)");
    puts("  --dump-regs       结束时转储寄存器");
    puts("  --stats           结束时打印执行指令数");
    puts("退出码: 被模拟程序退出码; 124=达指令上限; 1=装载/执行错误");
}

int emu_main(int argc, char **argv)
{
    const char *file = NULL;
    int binary = 0, dump = 0, stats = 0, demo = 0;
    uint32_t base = RAM_BASE;
    size_t ram_mib = 8;

    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "--demo")) demo = 1;
        else if (arg_is(argv[i], "--demo2")) demo = 2;
        else if (arg_is(argv[i], "--binary")) binary = 1;
        else if (arg_is(argv[i], "--dump-regs")) dump = 1;
        else if (arg_is(argv[i], "--stats")) stats = 1;
        else if (arg_is(argv[i], "--trace")) ;
        else if (arg_is(argv[i], "--help") || arg_is(argv[i], "-h")) { emu_usage(); return 0; }
        else if (strncmp(argv[i], "--trace-after=", 14) == 0) ;
        else if (strncmp(argv[i], "--max=", 6) == 0) ;
        else if (strncmp(argv[i], "--base=", 7) == 0)
            base = (uint32_t)strtoull(argv[i] + 7, NULL, 0);
        else if (strncmp(argv[i], "--ram=", 6) == 0)
            ram_mib = (size_t)strtoull(argv[i] + 6, NULL, 0);
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "oskit emu: 未知选项 %s\n", argv[i]);
            return 2;
        } else if (!file) {
            file = argv[i];
        } else {
            fprintf(stderr, "oskit emu: 多余参数 %s\n", argv[i]);
            return 2;
        }
    }
    if (!demo && !file) {
        emu_usage();
        return 2;
    }
    if (ram_mib == 0 || ram_mib > 256)
        die("--ram 取值 1..256 MiB");

    vm_t v;
    memset(&v, 0, sizeof v);
    v.ram_size = ram_mib << 20;
    v.ram = calloc(1, v.ram_size);
    if (!v.ram)
        die("无法分配 %zu MiB 模拟内存", ram_mib);
    v.max = 50000000ull;
    v.trace = 0;
    v.trace_after = 0;
    /* 第二遍: trace 相关(上面故意空分支保持参数序独立) */
    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "--trace")) v.trace = 1;
        else if (strncmp(argv[i], "--trace-after=", 14) == 0)
            v.trace_after = strtoull(argv[i] + 14, NULL, 0);
        else if (strncmp(argv[i], "--max=", 6) == 0)
            v.max = strtoull(argv[i] + 6, NULL, 0);
    }

    if (demo) {
        const uint32_t *code = demo == 1 ? g_demo1 : g_demo2;
        size_t words = demo == 1 ? sizeof g_demo1 / 4 : sizeof g_demo2 / 4;
        memcpy(v.ram, code, words * 4);
        v.pc = RAM_BASE;
        printf("[内置演示程序 demo%d, %zu 条指令, 入口 0x%08x]\n", demo, words, v.pc);
    } else {
        void *buf;
        size_t len;
        if (xread_file(file, &buf, &len) != 0)
            die("无法读取 %s: %s", file, strerror(errno));
        if (binary)
            load_flat(&v, buf, len, base);
        else
            load_elf(&v, buf, len);
        free(buf);
        printf("[程序 %s, 入口 pc=0x%08x]\n", file, v.pc);
    }

    while (!v.exited) {
        if (v.instret >= v.max) {
            printf("\n模拟器: 达到指令上限 %llu, 强制停止 (exit 124)\n",
                   (unsigned long long)v.max);
            if (dump || stats)
                printf("已执行 %llu 条\n", (unsigned long long)v.instret);
            free(v.ram);
            return 124;
        }
        vm_step(&v);
    }

    if (dump) {
        for (int i = 0; i < 32; i++) {
            printf("%3s=0x%08x", RN[i], v.x[i]);
            putchar(i % 4 == 3 ? '\n' : ' ');
        }
        printf(" pc=0x%08x\n", v.pc);
    }
    if (stats)
        printf("已执行 %llu 条指令\n", (unsigned long long)v.instret);
    free(v.ram);
    return v.exit_code;
}
