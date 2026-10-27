/* hello_frame.c —— oskit debug 的测试目标：保留帧指针, 便于 bt 演示
 * 构建: -fno-omit-frame-pointer (见 run_tests.sh) */
#include <stdio.h>

static int g_counter = 7;   /* watch 观察点演示目标 */

static void worker(const char *msg)
{
    printf("[worker] %s (counter=%d)\n", msg, g_counter);
    g_counter++;
    printf("[worker] counter -> %d\n", g_counter);
}

int main(int argc, char **argv)
{
    const char *greeting = "hello from oskit debug target";
    worker(greeting);
    printf("[main] done, counter=%d, argc=%d\n", g_counter, argc);
    return 0;
}
