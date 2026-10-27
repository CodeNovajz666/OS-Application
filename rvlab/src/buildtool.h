/* buildtool.h —— RISC-V 交叉工具链探测与构建管线 */
#ifndef RVLAB_BUILDTOOL_H
#define RVLAB_BUILDTOOL_H

#include <stddef.h>

/* 返回可用交叉编译器命令(优先 $RISCV_CC), 找不到返回 NULL */
const char *buildtool_find_cc(void);

typedef struct {
    char file[128];
    int  line;
    char sev;          /* 'e' 错误 'w' 警告 */
    char msg[200];
} problem_t;

/* 编译单文件(.c/.s) → out.elf; popen 捕获输出到 log(截断);
 * 解析出的诊断进 probs(≤max), 数量写 *nprobs。返回编译退出码(0 成功)。 */
int buildtool_build(const char *file, const char *out,
                    problem_t *probs, int max, int *nprobs,
                    char *log, size_t logsz);

#endif
