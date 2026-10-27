/* vuln_target.c —— oskit vuln 的测试目标：故意使用不安全函数与无加固编译
 * 构建脚本刻意使用: -fno-stack-protector -no-pie (见 run_tests.sh) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    char buf[64];
    char name[32];
    strcpy(name, "oskit-vuln-target");          /* 高危: 无界拷贝 */
    sprintf(buf, "%s-%d", name, argc);          /* 高危: 无界格式化 */
    scanf("%15s", name);                        /* 高危: scanf 家族(此处带了宽度) */
    system("echo vuln-target-ran");             /* 中危: 命令注入面 */
    char *tok = strtok(buf, "-");               /* 中危: 非可重入 */
    (void)tok;
    printf("%s\n", buf);
    return 0;
}
