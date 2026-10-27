/* rsp_client.c —— 纯 C 的 GDB-RSP 联测客户端(不依赖 gdb/python)
 * 用法: rsp_client <端口>  —— 对 rvlab --gdbstub --demo 1 做全链路验证:
 * qSupported → m 读入口指令 → s 单步 → g 验 pc → Z0 设断点 → c 命中 → k 结束 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail;

static void check(const char *name, int ok)
{
    printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    if (!ok)
        g_fail++;
}

static uint8_t csum(const char *p)
{
    uint8_t s = 0;
    while (*p) s += (uint8_t)*p++;
    return s;
}

static int send_pkt(int fd, const char *data)
{
    char buf[4096];
    int n = snprintf(buf, sizeof buf, "$%s#%02x", data, csum(data));
    return write(fd, buf, (size_t)n) == n ? 0 : -1;
}

/* 读一帧(跳过 +/-), 返回 data 长度, 失败 -1 */
static int read_pkt(int fd, char *out, size_t outsz)
{
    for (;;) {
        char c;
        if (read(fd, &c, 1) != 1)
            return -1;
        if (c == '+' || c == '-')
            continue;
        if (c != '$')
            continue;
        size_t n = 0;
        while (n + 1 < outsz) {
            if (read(fd, &c, 1) != 1)
                return -1;
            if (c == '#')
                break;
            out[n++] = c;
        }
        out[n] = 0;
        char cs[3];
        if (read(fd, cs, 2) != 2)
            return -1;
        cs[2] = 0;
        char ack = csum(out) == (uint8_t)strtol(cs, NULL, 16) ? '+' : '-';
        write(fd, &ack, 1);
        return (int)n;
    }
}

static int hx(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <端口>\n", argv[0]);
        return 2;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)atoi(argv[1]));
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        perror("connect");
        return 1;
    }
    char pkt[4096];

    /* 1. qSupported */
    send_pkt(fd, "qSupported:multiprocess+");
    read_pkt(fd, pkt, sizeof pkt);
    check("qSupported 应答", strncmp(pkt, "PacketSize=", 11) == 0);

    /* 2. 读入口 4 字节: demo1 首指令 0x100002B7 → 小端 b7 02 00 10 */
    send_pkt(fd, "m80000000,4");
    read_pkt(fd, pkt, sizeof pkt);
    check("m 读内存 = demo1 首指令", strcmp(pkt, "b7020010") == 0);

    /* 3. 单步 → T05 */
    send_pkt(fd, "s");
    read_pkt(fd, pkt, sizeof pkt);
    check("s 单步停(T05)", strncmp(pkt, "T05", 3) == 0);

    /* 4. g 包: 33 寄存器×4 字节, 末 4 字节为 pc → 0x80000004 */
    send_pkt(fd, "g");
    int n = read_pkt(fd, pkt, sizeof pkt);
    uint32_t pc = 0;
    if (n == 33 * 8) {
        for (int k = 0; k < 4; k++)
            pc |= (uint32_t)hx(pkt[32 * 8 + k * 2]) << (8 * k + 4) |
                  (uint32_t)hx(pkt[32 * 8 + k * 2 + 1]) << (8 * k);
        check("g 包长度与 pc=0x80000004", pc == 0x80000004u);
    } else {
        printf("     (g 长度 %d)\n", n);
        check("g 包长度 264", 0);
    }

    /* 5. Z0 断点在 demo1 末条 sw(0x80000044) → c 命中 T05 */
    send_pkt(fd, "Z0,80000044,4");
    read_pkt(fd, pkt, sizeof pkt);
    check("Z0 设断点", strcmp(pkt, "OK") == 0);
    send_pkt(fd, "c");
    read_pkt(fd, pkt, sizeof pkt);
    check("c 运行至断点(T05)", strncmp(pkt, "T05", 3) == 0);

    /* 6. 再 c → 越过断点执行 exit(42) → W2a */
    send_pkt(fd, "c");
    read_pkt(fd, pkt, sizeof pkt);
    check("c 续跑至退出(W2a)", strcmp(pkt, "W2a") == 0);

    /* 7. k 结束 */
    send_pkt(fd, "k");
    check("k 结束会话", 1);
    close(fd);

    printf("==== %s ====\n", g_fail ? "失败" : "全部通过");
    return g_fail ? 1 : 0;
}
