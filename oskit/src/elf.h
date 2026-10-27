/* elf.h —— 共享的最小 ELF 解析器（ELF32/ELF64 · 小端）
 * 被 vuln（安全检查）/ debug（符号解析）/ emu（程序装载）三个工具复用。
 * 只读、全边界校验：畸形文件返回错误，绝不越界访问。 */
#ifndef OSKIT_ELF_H
#define OSKIT_ELF_H

#include <stddef.h>
#include <stdint.h>

typedef struct elf_file elf_file;

/* 解析缓冲区（buf 归调用者所有，ef 关闭前须有效）。失败返回 NULL。 */
elf_file *elf_open_buf(const void *buf, size_t len);
void      elf_close(elf_file *ef);

int      elf_is64(const elf_file *ef);   /* 1=ELF64, 0=ELF32 */
uint16_t elf_type(const elf_file *ef);   /* ET_EXEC=2 / ET_DYN=3 ... */
uint16_t elf_machine(const elf_file *ef);/* EM_386=3 / EM_ARM=40 / EM_X86_64=62 / EM_RISCV=243 */
uint64_t elf_entry(const elf_file *ef);

/* ---------- 程序头 ---------- */
typedef struct {
    uint64_t type, flags;      /* PT_* / PF_* */
    uint64_t offset, vaddr, paddr, filesz, memsz;
} elf_phdr;

size_t elf_phdr_count(const elf_file *ef);
int    elf_phdr_get(const elf_file *ef, size_t i, elf_phdr *out);

/* ---------- 节头（名称已解析）---------- */
typedef struct {
    const char *name;
    uint32_t    type;          /* SHT_* */
    uint64_t    addr, offset, size, link;
} elf_shdr;

size_t elf_shdr_count(const elf_file *ef);
int    elf_shdr_get(const elf_file *ef, size_t i, elf_shdr *out);

/* ---------- 符号（.symtab 与 .dynsym）---------- */
#define ELF_SYM_BIND(info)  ((uint8_t)(info) >> 4)
#define ELF_SYM_TYPE(info)  ((uint8_t)(info) & 0xf)

/* 先遍历 .symtab（若未 strip）再遍历 .dynsym；回调返回非 0 提前终止 */
typedef int (*elf_sym_cb)(void *ud, const char *name, uint64_t addr,
                          uint64_t size, uint8_t info, uint16_t shndx);
void elf_for_each_sym(const elf_file *ef, elf_sym_cb cb, void *ud);

/* 按名查找已定义符号（shndx != 0），优先 .symtab；找到返回 0 */
int elf_sym_lookup(const elf_file *ef, const char *name,
                   uint64_t *addr, uint64_t *size);
/* 是否存在 .symtab（未 strip）*/
int elf_has_symtab(const elf_file *ef);

/* ---------- 动态段（PT_DYNAMIC）---------- */
typedef struct {
    int64_t  tag;
    uint64_t val;
} elf_dyn;

/* 最多复制 max 条（遇到 DT_NULL 停止），实际条数写入 *count；无动态段返回 -1 */
int elf_dynamic(const elf_file *ef, elf_dyn *out, size_t max, size_t *count);

/* 把文件内虚拟地址换算成文件偏移（经 PT_LOAD）；失败返回 -1 */
int elf_vaddr_to_off(const elf_file *ef, uint64_t vaddr, uint64_t *off);

/* 读取以 vaddr 开始的 NUL 结尾字符串（经 vaddr_to_off），越界/截断安全 */
int elf_read_cstr(const elf_file *ef, uint64_t vaddr, char *out, size_t outsz);

#endif
