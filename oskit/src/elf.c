/* elf.c —— 最小 ELF 解析器实现（ELF32/ELF64 · 小端宿主读小端文件）
 * 手工定义头结构而非包含 <elf.h>：同时支持 32/64、避免宿主宏差异。 */
#include "elf.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

/* ---- ELF 常量（本地定义，避免平台头差异）---- */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_DYNSYM  11
#define SHN_UNDEF   0

struct elf_file {
    const unsigned char *b;
    size_t len;
    int is64;
    uint16_t e_type, e_machine;
    uint64_t e_entry, e_phoff, e_shoff;
    uint16_t e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;

    elf_phdr *ph; size_t nph;           /* 已归一化的程序头 */
    elf_shdr *sh; size_t nsh;           /* 已归一化+命名的节头 */
    const char **shnames;               /* 每节名字（指向 buf 内或 NULL）*/

    /* 符号表：0=.symtab 1=.dynsym（索引到 sh 数组），-1 表示无 */
    int sym_idx[2];
};

/* 小端读取原语 */
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int in_bounds(const elf_file *ef, uint64_t off, uint64_t size)
{
    return off <= ef->len && size <= ef->len - off;
}

elf_file *elf_open_buf(const void *buf, size_t len)
{
    const unsigned char *b = buf;
    if (!buf || len < 0x34)
        return NULL;
    if (b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F')
        return NULL;
    if ((b[4] != 1 && b[4] != 2) || b[5] != 1)      /* EI_CLASS 1/2 且只支持小端 */
        return NULL;

    elf_file *ef = calloc(1, sizeof *ef);
    if (!ef)
        return NULL;
    ef->b = b;
    ef->len = len;
    ef->is64 = (b[4] == 2);

    const unsigned char *eh = b;
    ef->e_type    = rd16(eh + 16);
    ef->e_machine = rd16(eh + 18);
    ef->e_entry   = ef->is64 ? rd64(eh + 24) : rd32(eh + 24);
    ef->e_phoff   = ef->is64 ? rd64(eh + 32) : rd32(eh + 28);
    ef->e_shoff   = ef->is64 ? rd64(eh + 40) : rd32(eh + 32);
    ef->e_phentsize = rd16(eh + (ef->is64 ? 54 : 42));
    ef->e_phnum     = rd16(eh + (ef->is64 ? 56 : 44));
    ef->e_shentsize = rd16(eh + (ef->is64 ? 58 : 46));
    ef->e_shnum     = rd16(eh + (ef->is64 ? 60 : 48));
    ef->e_shstrndx  = rd16(eh + (ef->is64 ? 62 : 50));

    /* ---------- 程序头 ---------- */
    if (ef->e_phnum && ef->e_phoff) {
        size_t entsz = ef->is64 ? 56 : 32;
        if (ef->e_phentsize < entsz)
            goto fail;
        ef->ph = calloc(ef->e_phnum, sizeof *ef->ph);
        if (!ef->ph) goto fail;
        for (uint16_t i = 0; i < ef->e_phnum; i++) {
            uint64_t off = ef->e_phoff + (uint64_t)i * ef->e_phentsize;
            if (!in_bounds(ef, off, entsz)) goto fail;
            const unsigned char *p = b + off;
            elf_phdr *ph = &ef->ph[i];
            if (ef->is64) {
                ph->type   = rd32(p);
                ph->flags  = rd32(p + 4);
                ph->offset = rd64(p + 8);
                ph->vaddr  = rd64(p + 16);
                ph->paddr  = rd64(p + 24);
                ph->filesz = rd64(p + 32);
                ph->memsz  = rd64(p + 40);
            } else {
                ph->type   = rd32(p);
                ph->offset = rd32(p + 4);
                ph->vaddr  = rd32(p + 8);
                ph->paddr  = rd32(p + 12);
                ph->filesz = rd32(p + 16);
                ph->memsz  = rd32(p + 20);
                ph->flags  = rd32(p + 24);
            }
            ef->nph++;
        }
    }

    /* ---------- 节头 ---------- */
    if (ef->e_shnum && ef->e_shoff) {
        size_t entsz = ef->is64 ? 64 : 40;
        if (ef->e_shentsize < entsz)
            goto fail;
        ef->sh = calloc(ef->e_shnum, sizeof *ef->sh);
        ef->shnames = calloc(ef->e_shnum, sizeof(const char *));
        if (!ef->sh || !ef->shnames) goto fail;
        for (uint16_t i = 0; i < ef->e_shnum; i++) {
            uint64_t off = ef->e_shoff + (uint64_t)i * ef->e_shentsize;
            if (!in_bounds(ef, off, entsz)) goto fail;
            const unsigned char *p = b + off;
            uint32_t nameoff;
            elf_shdr sh;
            memset(&sh, 0, sizeof sh);
            if (ef->is64) {
                nameoff   = rd32(p);
                sh.type   = rd32(p + 4);
                sh.addr   = rd64(p + 16);
                sh.offset = rd64(p + 24);
                sh.size   = rd64(p + 32);
                sh.link   = rd32(p + 40);  /* ELF64 中 sh_link/sh_info 均为 4 字节 */
            } else {
                nameoff   = rd32(p);
                sh.type   = rd32(p + 4);
                sh.addr   = rd32(p + 12);
                sh.offset = rd32(p + 16);
                sh.size   = rd32(p + 20);
                sh.link   = rd32(p + 24);
            }
            ef->sh[i] = sh;
            ef->shnames[i] = (const char *)(uintptr_t)(size_t)nameoff; /* 暂存 nameoff，稍后解析 */
            ef->nsh++;
        }
        /* 用 .shstrtab 解析节名 */
        if (ef->e_shstrndx < ef->nsh) {
            const elf_shdr *ss = &ef->sh[ef->e_shstrndx];
            if (ss->type == SHT_STRTAB && in_bounds(ef, ss->offset, ss->size) && ss->size) {
                const char *strs = (const char *)b + ss->offset;
                size_t strsz = (size_t)ss->size;
                for (size_t i = 0; i < ef->nsh; i++) {
                    size_t no = (size_t)(uintptr_t)ef->shnames[i];
                    ef->shnames[i] = (no < strsz && strsz && strs[strsz - 1] == 0)
                                     ? strs + no : NULL;
                    /* 越界名字一律置 NULL，调用方按无名处理 */
                    if (ef->shnames[i]) {
                        /* 确认到 NUL 为止都在节内 */
                        const char *q = ef->shnames[i];
                        size_t rem = strsz - no;
                        size_t k = 0;
                        while (k < rem && q[k]) k++;
                        if (k >= rem) ef->shnames[i] = NULL;
                    }
                }
            } else {
                for (size_t i = 0; i < ef->nsh; i++) ef->shnames[i] = NULL;
            }
        }
        /* 定位符号表 */
        ef->sym_idx[0] = ef->sym_idx[1] = -1;
        for (size_t i = 0; i < ef->nsh; i++) {
            if (ef->sh[i].type == SHT_SYMTAB && ef->sym_idx[0] < 0)
                ef->sym_idx[0] = (int)i;
            if (ef->sh[i].type == SHT_DYNSYM && ef->sym_idx[1] < 0)
                ef->sym_idx[1] = (int)i;
        }
    } else {
        ef->sym_idx[0] = ef->sym_idx[1] = -1;
    }
    return ef;

fail:
    elf_close(ef);
    return NULL;
}

void elf_close(elf_file *ef)
{
    if (!ef) return;
    free(ef->ph);
    free(ef->sh);
    free(ef->shnames);
    free(ef);
}

int      elf_is64(const elf_file *ef)   { return ef->is64; }
uint16_t elf_type(const elf_file *ef)   { return ef->e_type; }
uint16_t elf_machine(const elf_file *ef){ return ef->e_machine; }
uint64_t elf_entry(const elf_file *ef)  { return ef->e_entry; }
size_t   elf_phdr_count(const elf_file *ef) { return ef->nph; }
size_t   elf_shdr_count(const elf_file *ef) { return ef->nsh; }

int elf_phdr_get(const elf_file *ef, size_t i, elf_phdr *out)
{
    if (i >= ef->nph) return -1;
    *out = ef->ph[i];
    return 0;
}

int elf_shdr_get(const elf_file *ef, size_t i, elf_shdr *out)
{
    if (i >= ef->nsh) return -1;
    *out = ef->sh[i];
    out->name = ef->shnames[i];
    return 0;
}

int elf_has_symtab(const elf_file *ef) { return ef->sym_idx[0] >= 0; }

/* 遍历一个符号表节；返回回调非 0 的值（0=全部跑完） */
static int iter_symtab(const elf_file *ef, int shidx, elf_sym_cb cb, void *ud)
{
    if (shidx < 0 || (size_t)shidx >= ef->nsh)
        return 0;
    const elf_shdr *sym = &ef->sh[shidx];
    if (sym->link >= ef->nsh)
        return 0;
    const elf_shdr *str = &ef->sh[sym->link];
    if (str->type != SHT_STRTAB || !in_bounds(ef, str->offset, str->size) || str->size == 0)
        return 0;
    const char *strs = (const char *)ef->b + str->offset;
    size_t strsz = (size_t)str->size;

    size_t entsz = ef->is64 ? 24 : 16;
    if (sym->size < entsz || !in_bounds(ef, sym->offset, sym->size))
        return 0;
    size_t cnt = (size_t)(sym->size / entsz);
    for (size_t i = 0; i < cnt; i++) {
        const unsigned char *p = ef->b + sym->offset + i * entsz;
        uint32_t nameoff = rd32(p);
        uint8_t  info;
        uint16_t shndx;
        uint64_t val, size;
        if (ef->is64) {
            info   = p[4];
            shndx  = rd16(p + 6);
            val    = rd64(p + 8);
            size   = rd64(p + 16);
        } else {
            val    = rd32(p + 4);
            size   = rd32(p + 8);
            info   = p[12];
            shndx  = rd16(p + 14);
        }
        const char *name = NULL;
        if (nameoff < strsz) {
            const char *q = strs + nameoff;
            size_t rem = strsz - nameoff, k = 0;
            while (k < rem && q[k]) k++;
            if (k < rem || (k == rem && q[rem - 1] == 0 && rem > 0))
                name = (nameoff == 0) ? "" : q;
        }
        if (!name || !*name)
            continue;
        int r = cb(ud, name, val, size, info, shndx);
        if (r)
            return r;
    }
    return 0;
}

void elf_for_each_sym(const elf_file *ef, elf_sym_cb cb, void *ud)
{
    if (iter_symtab(ef, ef->sym_idx[0], cb, ud))
        return;
    iter_symtab(ef, ef->sym_idx[1], cb, ud);
}

typedef struct {
    const char *want;
    uint64_t addr, size;
    int found;
} lookup_ud;

static int lookup_cb(void *ud, const char *name, uint64_t addr, uint64_t size,
                     uint8_t info, uint16_t shndx)
{
    (void)info;
    lookup_ud *l = ud;
    if (shndx != SHN_UNDEF && strcmp(name, l->want) == 0) {
        l->found = 1;
        l->addr = addr;
        l->size = size;
        return 1;
    }
    return 0;
}

int elf_sym_lookup(const elf_file *ef, const char *name, uint64_t *addr, uint64_t *size)
{
    lookup_ud l = { name, 0, 0, 0 };
    elf_for_each_sym(ef, lookup_cb, &l);
    if (!l.found)
        return -1;
    *addr = l.addr;
    *size = l.size;
    return 0;
}

int elf_dynamic(const elf_file *ef, elf_dyn *out, size_t max, size_t *count)
{
    for (size_t i = 0; i < ef->nph; i++) {
        if (ef->ph[i].type != PT_DYNAMIC)
            continue;
        size_t entsz = ef->is64 ? 16 : 8;
        if (!in_bounds(ef, ef->ph[i].offset, ef->ph[i].filesz) || ef->ph[i].filesz < entsz) {
            *count = 0;
            return 0; /* 畸形动态段按空处理 */
        }
        size_t cnt = (size_t)(ef->ph[i].filesz / entsz);
        size_t n = 0;
        const unsigned char *p = ef->b + ef->ph[i].offset;
        for (size_t k = 0; k < cnt && n < max; k++, p += entsz) {
            int64_t tag = ef->is64 ? (int64_t)rd64(p) : (int64_t)(int32_t)rd32(p);
            uint64_t val = ef->is64 ? rd64(p + 8) : rd32(p + 8);
            if (tag == 0) /* DT_NULL */
                break;
            out[n].tag = tag;
            out[n].val = val;
            n++;
        }
        *count = n;
        return 0;
    }
    return -1;
}

int elf_vaddr_to_off(const elf_file *ef, uint64_t vaddr, uint64_t *off)
{
    for (size_t i = 0; i < ef->nph; i++) {
        const elf_phdr *p = &ef->ph[i];
        if (p->type != PT_LOAD)
            continue;
        if (vaddr >= p->vaddr && vaddr - p->vaddr < p->filesz) {
            uint64_t o = p->offset + (vaddr - p->vaddr);
            if (!in_bounds(ef, o, 1))
                return -1;
            *off = o;
            return 0;
        }
    }
    return -1;
}

int elf_read_cstr(const elf_file *ef, uint64_t vaddr, char *out, size_t outsz)
{
    uint64_t off;
    if (outsz == 0 || elf_vaddr_to_off(ef, vaddr, &off) != 0)
        return -1;
    size_t i = 0;
    while (i + 1 < outsz) {
        if (off + i >= ef->len)
            break;
        char c = (char)ef->b[off + i++];
        *out++ = c;
        if (c == 0) { out[-1] = 0; return 0; }
    }
    *out = 0;
    return 0; /* 允许截断，保证 NUL 结尾 */
}
