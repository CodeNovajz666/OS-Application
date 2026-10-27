/* skeleton.h —— 裸机工程骨架生成(riscv32/riscv64, 模板源自 ../oskit port) */
#ifndef RVLAB_SKELETON_H
#define RVLAB_SKELETON_H

/* target: "riscv32"|"riscv64" · 在 dir 下创建 <name>/ 六个文件
 * 返回 0 成功; -1 参数错; -2 目录已存在; -3 写失败 */
int skeleton_gen(const char *target, const char *name, const char *dir);

#endif
