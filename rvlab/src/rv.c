/* rv.c —— RV32I+M 虚拟机实现（内核执行逻辑移植自 ../oskit/src/emu.c） */
#include "rv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *RN[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static const char *CLS[RV_OP_CLASS_N] = {
    "lui", "auipc", "jal", "jalr", "branch",
    "load", "store", "op-imm", "op", "fence", "system", "muldiv",
};

const char *rv_reg_name(int r)      { return (r >= 0 && r < 32) ? RN[r] : "?"; }
const char *rv_op_class_name(int c) { return (c >= 0 && c < RV_OP_CLASS_N) ? CLS[c] : "?"; }

static uint32_t sext(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

static void set_reg(rv_vm *v, unsigned rd, uint32_t val)
{
    if (rd) v->x[rd] = val;
}

/* ---------- 地址分类 / MMIO ---------- */
enum { A_RAM, A_UART, A_MTIME, A_EXIT, A_FAULT };

static int addr_kind(const rv_vm *v, uint32_t a)
{
    if (a >= RV_RAM_BASE && (uint64_t)a < (uint64_t)RV_RAM_BASE + v->ram_size) return A_RAM;
    if (a >= RV_UART_BASE && a < RV_UART_BASE + 0x100) return A_UART;
    if (a >= RV_MTIME_ADDR && a < RV_MTIME_ADDR + 8)   return A_MTIME;
    if (a >= RV_EXIT_BASE && a < RV_EXIT_BASE + 8)     return A_EXIT;
    return A_FAULT;
}

static uint8_t vm_ld8(rv_vm *v, uint32_t a, int *ok)
{
    switch (addr_kind(v, a)) {
    case A_RAM:   *ok = 1; return v->ram[a - RV_RAM_BASE];
    case A_UART:  *ok = 1; return (a & 0xff) == 0x05 ? 0x20 : 0;
    case A_MTIME: {
        *ok = 1;
        uint64_t t = v->instret;
        return (uint8_t)(t >> (8 * (int)(a - RV_MTIME_ADDR)));
    }
    default: *ok = 0; return 0;
    }
}

static void vm_st8(rv_vm *v, uint32_t a, uint8_t val)
{
    switch (addr_kind(v, a)) {
    case A_RAM: v->ram[a - RV_RAM_BASE] = val; break;
    case A_UART:
        if ((a & 0xff) == 0x00 && v->uart_fn)
            v->uart_fn(v->uart_ud, val);
        break;
    case A_EXIT:
        if ((a & 7) == 0) { v->exited = 1; v->exit_code = val & 0xff; }
        break;
    default: break; /* 宽松: 忽略未映射写 */
    }
}

int rv_peek(rv_vm *v, uint32_t addr, uint8_t *bytes, size_t n)
{
    if (addr_kind(v, addr) != A_RAM ||
        (uint64_t)addr + n > (uint64_t)RV_RAM_BASE + v->ram_size)
        return -1;
    memcpy(bytes, v->ram + (addr - RV_RAM_BASE), n);
    return 0;
}

int rv_poke(rv_vm *v, uint32_t addr, const uint8_t *bytes, size_t n)
{
    if (addr_kind(v, addr) != A_RAM ||
        (uint64_t)addr + n > (uint64_t)RV_RAM_BASE + v->ram_size)
        return -1;
    memcpy(v->ram + (addr - RV_RAM_BASE), bytes, n);
    return 0;
}

/* ---------- 生命周期 ---------- */
rv_vm *rv_create(size_t ram_mib)
{
    if (ram_mib == 0 || ram_mib > 256)
        return NULL;
    rv_vm *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->ram_size = ram_mib << 20;
    v->ram = calloc(1, v->ram_size);
    if (!v->ram) { free(v); return NULL; }
    v->pc = RV_RAM_BASE;
    return v;
}

void rv_free(rv_vm *v)
{
    if (!v) return;
    free(v->ram);
    free(v->syms);
    free(v->sym_count);
    free(v);
}

void rv_reset(rv_vm *v, uint32_t entry)
{
    memset(v->x, 0, sizeof v->x);
    v->pc = entry;
    v->instret = 0;
    v->exited = 0;
    v->exit_code = 0;
    v->hit_bp = 0;
    memset(v->op_count, 0, sizeof v->op_count);
    v->br_total = v->br_taken = v->load_cnt = v->store_cnt = 0;
    if (v->sym_count) memset(v->sym_count, 0, v->n_syms * sizeof(uint64_t));
}

int rv_bp_add(rv_vm *v, uint64_t addr)
{
    if (v->n_bp >= 64) return -1;
    v->bp[v->n_bp++] = addr;
    return 0;
}

void rv_bp_clear(rv_vm *v) { v->n_bp = 0; }

static int bp_at(const rv_vm *v, uint32_t pc)
{
    for (int i = 0; i < v->n_bp; i++)
        if ((uint32_t)v->bp[i] == pc)
            return 1;
    return 0;
}

void rv_set_syms(rv_vm *v, const rv_sym *tab, size_t n)
{
    free(v->syms);
    free(v->sym_count);
    v->n_syms = 0;
    v->syms = NULL;
    v->sym_count = NULL;
    if (!tab || !n) return;
    v->syms = malloc(n * sizeof *v->syms);
    v->sym_count = calloc(n, sizeof *v->sym_count);
    if (!v->syms || !v->sym_count) {
        free(v->syms); free(v->sym_count);
        v->syms = NULL; v->sym_count = NULL;
        return;
    }
    memcpy(v->syms, tab, n * sizeof *v->syms);
    v->n_syms = n;
}

/* ---------- ELF 装载（依赖 ../oskit 的共享解析器）---------- */
#include "elf.h"

int rv_load_elf(rv_vm *v, const void *buf, size_t len)
{
    elf_file *ef = elf_open_buf(buf, len);
    if (!ef) { snprintf(v->err, sizeof v->err, "非法 ELF(仅小端 ELF32/ELF64)"); return RV_ERR; }
    if (elf_is64(ef)) {
        snprintf(v->err, sizeof v->err, "ELF64 程序, 本 VM 为 RV32 (用 -march=rv32... 重编)");
        elf_close(ef);
        return RV_ERR;
    }
    if (elf_machine(ef) != 243) {
        snprintf(v->err, sizeof v->err, "e_machine=%u 非 RISC-V", elf_machine(ef));
        elf_close(ef);
        return RV_ERR;
    }
    size_t nph = elf_phdr_count(ef);
    for (size_t i = 0; i < nph; i++) {
        elf_phdr ph;
        elf_phdr_get(ef, i, &ph);
        if (ph.type != 1)
            continue;
        if (ph.vaddr < RV_RAM_BASE ||
            (uint64_t)ph.vaddr + ph.memsz > (uint64_t)RV_RAM_BASE + v->ram_size) {
            snprintf(v->err, sizeof v->err, "PT_LOAD [0x%llx,0x%llx) 超出 RAM",
                     (unsigned long long)ph.vaddr, (unsigned long long)ph.memsz);
            elf_close(ef);
            return RV_ERR;
        }
        memcpy(v->ram + (ph.vaddr - RV_RAM_BASE),
               (const uint8_t *)buf + ph.offset, (size_t)ph.filesz);
    }
    if (elf_entry(ef) < RV_RAM_BASE || elf_entry(ef) - RV_RAM_BASE >= v->ram_size) {
        snprintf(v->err, sizeof v->err, "入口 0x%llx 不在 RAM", (unsigned long long)elf_entry(ef));
        elf_close(ef);
        return RV_ERR;
    }
    v->pc = (uint32_t)elf_entry(ef);
    elf_close(ef);
    return 0;
}

int rv_load_flat(rv_vm *v, const void *buf, size_t len, uint32_t base)
{
    if (base != RV_RAM_BASE || len > v->ram_size) {
        snprintf(v->err, sizeof v->err, "平坦二进制需 base=0x80000000 且 ≤ RAM");
        return RV_ERR;
    }
    memcpy(v->ram, buf, len);
    v->pc = base;
    return 0;
}

/* ---------- 符号热点归因(升序表二分) ---------- */
static void attr_sym(rv_vm *v, uint32_t pc)
{
    if (!v->syms || !v->n_syms)
        return;
    size_t lo = 0, hi = v->n_syms;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v->syms[mid].addr <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return; /* pc 在首个符号之前 */
    size_t i = lo - 1;
    uint64_t off = pc - (uint32_t)v->syms[i].addr;
    if (off < (v->syms[i].size ? v->syms[i].size : 4))
        v->sym_count[i]++;
}

/* ---------- 单步执行 ----------
 * exec_one: 不做断点检查的裸执行; rv_step = 断点检查 + exec_one;
 * rv_run   = (若正停在断点先越过一条) + 循环{断点检查→执行} */
static int exec_one(rv_vm *v)
{
    if (v->exited)
        return RV_EXIT;
    /* 取指 */
    if ((v->pc & 3) || addr_kind(v, v->pc) != A_RAM) {
        snprintf(v->err, sizeof v->err, "取指失败 pc=0x%08x(未映射/未对齐)", v->pc);
        return RV_ERR;
    }
    uint32_t in;
    memcpy(&in, v->ram + (v->pc - RV_RAM_BASE), 4);

    unsigned op = in & 0x7f;
    unsigned rd = (in >> 7) & 31, f3 = (in >> 12) & 7;
    unsigned rs1 = (in >> 15) & 31, rs2 = (in >> 20) & 31, f7 = in >> 25;
    uint32_t a = v->x[rs1], b = v->x[rs2];
    int32_t immI = (int32_t)in >> 20;
    int32_t immS = ((int32_t)(in & 0xfe000000) >> 20) | (int32_t)((in >> 7) & 0x1f);
    int32_t immB = (int32_t)sext(((in >> 31) << 12) | (((in >> 7) & 1) << 11) |
                                 (((in >> 25) & 0x3f) << 5) | (((in >> 8) & 0xf) << 1), 13);
    uint32_t immU = in & 0xfffff000u;
    int32_t immJ = (int32_t)sext(((in >> 31) << 20) | (((in >> 12) & 0xff) << 12) |
                                 (((in >> 20) & 1) << 11) | (((in >> 21) & 0x3ff) << 1), 21);
    uint32_t next = v->pc + 4;
    int cls = -1;

    switch (op) {
    case 0x37: set_reg(v, rd, immU); cls = RV_OP_LUI; break;
    case 0x17: set_reg(v, rd, v->pc + immU); cls = RV_OP_AUIPC; break;
    case 0x6f: set_reg(v, rd, v->pc + 4); next = v->pc + (uint32_t)immJ; cls = RV_OP_JAL; break;
    case 0x67: {
        uint32_t t = v->pc + 4;
        next = (a + (uint32_t)immI) & ~1u;
        set_reg(v, rd, t);
        cls = RV_OP_JALR;
        break;
    }
    case 0x63: {
        int take;
        switch (f3) {
        case 0: take = a == b; break;
        case 1: take = a != b; break;
        case 4: take = (int32_t)a <  (int32_t)b; break;
        case 5: take = (int32_t)a >= (int32_t)b; break;
        case 6: take = a <  b; break;
        case 7: take = a >= b; break;
        default:
            snprintf(v->err, sizeof v->err, "非法分支 f3=%u pc=0x%08x", f3, v->pc);
            return RV_ERR;
        }
        if (take) next = v->pc + (uint32_t)immB;
        v->br_total++;
        if (take) v->br_taken++;
        cls = RV_OP_BRANCH;
        break;
    }
    case 0x03: {
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
        case 2: {
            uint8_t bs[4];
            for (int k = 0; k < 4 && o; k++) bs[k] = vm_ld8(v, addr + (uint32_t)k, &o);
            if (o) memcpy(&val, bs, 4);
            break;
        }
        case 4: val = vm_ld8(v, addr, &o); break;
        case 5: {
            uint8_t lo = vm_ld8(v, addr, &o), hi = o ? vm_ld8(v, addr + 1, &o) : 0;
            val = lo | ((uint32_t)hi << 8);
            break;
        }
        default:
            snprintf(v->err, sizeof v->err, "非法加载 f3=%u pc=0x%08x", f3, v->pc);
            return RV_ERR;
        }
        if (!o) {
            snprintf(v->err, sizeof v->err, "加载未映射地址 0x%08x pc=0x%08x", addr, v->pc);
            return RV_ERR;
        }
        set_reg(v, rd, val);
        v->load_cnt++;
        cls = RV_OP_LOAD;
        break;
    }
    case 0x23: {
        uint32_t addr = a + (uint32_t)immS;
        switch (f3) {
        case 0: vm_st8(v, addr, (uint8_t)b); break;
        case 1: vm_st8(v, addr, (uint8_t)b); vm_st8(v, addr + 1, (uint8_t)(b >> 8)); break;
        case 2: {
            uint8_t bs[4];
            memcpy(bs, &b, 4);
            for (int k = 0; k < 4; k++) vm_st8(v, addr + (uint32_t)k, bs[k]);
            break;
        }
        default:
            snprintf(v->err, sizeof v->err, "非法存储 f3=%u pc=0x%08x", f3, v->pc);
            return RV_ERR;
        }
        v->store_cnt++;
        cls = RV_OP_STORE;
        break;
    }
    case 0x13: {
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
            else { snprintf(v->err, sizeof v->err, "非法移位 pc=0x%08x", v->pc); return RV_ERR; }
            break;
        case 6: r = a | imm; break;
        case 7: r = a & imm; break;
        }
        set_reg(v, rd, r);
        cls = RV_OP_OPIMM;
        break;
    }
    case 0x33: {
        uint32_t r = 0;
        if (f7 == 1) {
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
            cls = RV_OP_MULDIV;
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
            cls = RV_OP_OP;
        } else {
            snprintf(v->err, sizeof v->err, "非法 funct7=%u pc=0x%08x", f7, v->pc);
            return RV_ERR;
        }
        set_reg(v, rd, r);
        break;
    }
    case 0x0f: cls = RV_OP_FENCE; break;
    case 0x73: {
        if (f3 == 0) {
            if (in == 0x00000073) {                       /* ECALL */
                uint32_t nr = v->x[17], a0 = v->x[10];
                if (nr == 93) {
                    v->exited = 1;
                    v->exit_code = (int)(a0 & 0xff);
                    v->op_count[RV_OP_SYSTEM]++;
                    v->x[0] = 0;
                    v->pc = next;
                    v->instret++;
                    return RV_EXIT;
                } else if (nr == 64) {                    /* write(fd,buf,len) */
                    uint32_t ba = v->x[11], l = v->x[12];
                    if (addr_kind(v, ba) != A_RAM ||
                        (uint64_t)ba + l > (uint64_t)RV_RAM_BASE + v->ram_size) {
                        snprintf(v->err, sizeof v->err, "ecall write 参数越界");
                        return RV_ERR;
                    }
                    if (v->uart_fn)
                        for (uint32_t k = 0; k < l; k++)
                            v->uart_fn(v->uart_ud, v->ram[ba - RV_RAM_BASE + k]);
                    set_reg(v, 10, l);
                } else {
                    snprintf(v->err, sizeof v->err, "未实现 ecall a7=%u (支持 93/64)", nr);
                    return RV_ERR;
                }
            } else if (in == 0x00100073) {                /* EBREAK */
                snprintf(v->err, sizeof v->err, "ebreak @0x%08x", v->pc);
                v->op_count[RV_OP_SYSTEM]++;
                return RV_ERR;
            } else {
                snprintf(v->err, sizeof v->err, "非法 SYSTEM 指令 pc=0x%08x", v->pc);
                return RV_ERR;
            }
        } else {                                          /* CSR: 计数器只读 */
            set_reg(v, rd, 0);
        }
        cls = RV_OP_SYSTEM;
        break;
    }
    default:
        snprintf(v->err, sizeof v->err, "非法指令 0x%08x pc=0x%08x", in, v->pc);
        return RV_ERR;
    }

    if (cls >= 0) v->op_count[cls]++;
    v->x[0] = 0;
    uint32_t cur_pc = v->pc;      /* 本条指令自身的地址, 供归因/跟踪 */
    v->pc = next;
    v->instret++;
    attr_sym(v, cur_pc);
    if (v->trace_fn)
        v->trace_fn(v->trace_ud, cur_pc, in);
    if (v->exited)
        return RV_EXIT;
    return RV_OK;
}

int rv_step(rv_vm *v)
{
    if (v->exited)
        return RV_EXIT;
    if (bp_at(v, v->pc)) {
        v->hit_bp = v->pc;
        return RV_BREAK;
    }
    return exec_one(v);
}

int rv_run(rv_vm *v, uint64_t max_steps)
{
    if (v->exited)
        return RV_EXIT;
    if (bp_at(v, v->pc)) {                 /* 从断点上继续: 先越过本条 */
        v->hit_bp = 0;
        int r = exec_one(v);
        if (r != RV_OK)
            return r;
    }
    for (uint64_t i = 0; i < max_steps; i++) {
        if (v->exited)
            return RV_EXIT;
        if (bp_at(v, v->pc)) {
            v->hit_bp = v->pc;
            return RV_BREAK;
        }
        int r = exec_one(v);
        if (r != RV_OK)
            return r;
    }
    return v->exited ? RV_EXIT : RV_MAXSTEPS;
}

/* ---------- 轻量反汇编（与 oskit emu 同源）---------- */
void rv_disasm(uint32_t in, char *buf, size_t n)
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
