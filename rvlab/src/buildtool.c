/* buildtool.c —— 工具链探测与构建实现 */
#include "buildtool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_cc_cache;
static int g_cc_probed;

const char *buildtool_find_cc(void)
{
    if (g_cc_probed)
        return g_cc_cache;
    g_cc_probed = 1;
    static const char *cand[] = {
        /* 环境变量优先, 再按常见发行版包名顺序探测 */
        NULL, /* 占位: $RISCV_CC 动态取 */
        "riscv64-unknown-elf-gcc",
        "riscv64-linux-gnu-gcc",
        "riscv32-unknown-elf-gcc",
        "riscv-none-elf-gcc",
        "riscv64-unknown-linux-gnu-gcc",
    };
    const char *env = getenv("RISCV_CC");
    if (env && *env) {
        g_cc_cache = env;
        return env;
    }
    char cmd[256];
    for (size_t i = 1; i < sizeof cand / sizeof cand[0]; i++) {
        snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", cand[i]);
        if (system(cmd) == 0) {
            g_cc_cache = cand[i];
            return cand[i];
        }
    }
    return NULL;
}

/* 解析形如 "file.c:12:3: error: xxx" / "file.s:8: Error: xxx" 的诊断行 */
static void parse_diag(const char *line, problem_t *probs, int max, int *n)
{
    if (*n >= max)
        return;
    const char *p = line;
    /* 文件名: 到第一个冒号 */
    const char *c1 = strchr(p, ':');
    if (!c1 || c1 == p || (size_t)(c1 - p) >= sizeof probs[0].file)
        return;
    char file[128];
    memcpy(file, p, (size_t)(c1 - p));
    file[c1 - p] = 0;
    const char *c2 = strchr(c1 + 1, ':');
    if (!c2)
        return;
    char *end;
    long line_no = strtol(c1 + 1, &end, 10);
    if (end != c2 || line_no <= 0)
        return;
    /* 跳过可选的列号 ":%d:" */
    const char *msg = c2 + 1;
    const char *c3 = strchr(msg, ':');
    if (c3) {
        char *e2;
        long col = strtol(msg, &e2, 10);
        if (e2 == c3 && col > 0)
            msg = c3 + 1;
    }
    while (*msg == ' ')
        msg++;
    char sev = 'i';
    if (strstr(msg, "error") || strstr(msg, "Error"))
        sev = 'e';
    else if (strstr(msg, "warning") || strstr(msg, "Warning"))
        sev = 'w';
    if (sev == 'i' && !strstr(msg, "note"))
        return; /* 非诊断行 */
    snprintf(probs[*n].file, sizeof probs[0].file, "%s", file);
    probs[*n].line = (int)line_no;
    probs[*n].sev = sev;
    snprintf(probs[*n].msg, sizeof probs[0].msg, "%s", msg);
    (*n)++;
}

int buildtool_build(const char *file, const char *out,
                    problem_t *probs, int max, int *nprobs,
                    char *log, size_t logsz)
{
    *nprobs = 0;
    if (logsz) log[0] = 0;
    const char *cc = buildtool_find_cc();
    if (!cc)
        return -1;
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "%s -march=rv32im -mabi=ilp32 -nostdlib -static "
             "-Wl,-Ttext=0x80000000 -o '%s' '%s' 2>&1",
             cc, out, file);
    FILE *p = popen(cmd, "r");
    if (!p)
        return -2;
    char line[512];
    size_t used = 0;
    while (fgets(line, sizeof line, p)) {
        if (probs) parse_diag(line, probs, max, nprobs);
        if (log && used + 1 < logsz) {
            size_t l = strlen(line);
            if (used + l >= logsz) l = logsz - used - 1;
            memcpy(log + used, line, l);
            used += l;
            log[used] = 0;
        }
    }
    int rc = pclose(p);
    return rc == -1 ? -3 : (rc >> 8);
}
