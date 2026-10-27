/* gdbstub.c —— GDB RSP 服务实现
 * 寄存器布局(rv32): x0..x31 + pc, 每寄存器 4 字节小端(客户端需
 * `set architecture riscv:rv32`, gdb-multiarch 默认即兼容)。
 * 支持包: ? g G p P m M s c Z0 z0 H qSupported k D, 其余回空(不支持)。 */
#include "gdbstub.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PKT_MAX 4096

static int send_all(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w <= 0)
            return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static uint8_t csum(const char *p)
{
    uint8_t s = 0;
    while (*p)
        s += (uint8_t)*p++;
    return s;
}

static int send_pkt(int fd, const char *data)
{
    char head[2] = { '$', 0 };
    char tail[8];
    snprintf(tail, sizeof tail, "#%02x", csum(data));
    if (send_all(fd, head, 1) != 0) return -1;
    if (send_all(fd, data, strlen(data)) != 0) return -1;
    if (send_all(fd, tail, 3) != 0) return -1;
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint64_t hex2u64(const char *s, const char **end)
{
    uint64_t v = 0;
    while (*s && hexval(*s) >= 0) {
        v = (v << 4) | (uint64_t)hexval(*s);
        s++;
    }
    if (end)
        *end = s;
    return v;
}

static void reg_get(app_t *a, int idx, uint32_t *out)
{
    if (!a->vm) { *out = 0; return; }
    *out = idx < 32 ? a->vm->x[idx] : a->vm->pc;
}

static void reg_set(app_t *a, int idx, uint32_t val)
{
    if (!a->vm)
        return;
    if (idx < 32) {
        if (idx) a->vm->x[idx] = val;
    } else {
        a->vm->pc = val;
    }
}

/* 在断点上执行一条(临时摘除再放回) —— 供 gdb 的 s/c 语义使用 */
static int step_over_bp(app_t *a)
{
    rv_vm *v = a->vm;
    if (!v)
        return RV_ERR;
    if (v->n_bp) {
        for (int i = 0; i < v->n_bp; i++) {
            if ((uint32_t)v->bp[i] == v->pc) {
                uint64_t addr = v->bp[i];
                v->bp[i] = v->bp[--v->n_bp];
                int r = rv_step(v);            /* 此处 pc 无断点, 真执行 */
                rv_bp_add(v, addr);
                return r;
            }
        }
    }
    return rv_step(v);
}

static void stop_reply(char *out, size_t n, int r, app_t *a)
{
    if (r == RV_EXIT)
        snprintf(out, n, "W%02x", (unsigned)a->vm->exit_code & 0xff);
    else if (r == RV_ERR)
        snprintf(out, n, "E01;%s", a->vm->err);
    else
        snprintf(out, n, "T05");               /* 断点/单步/暂停 统一 SIGTRAP */
}

int gdbstub_serve(app_t *a, int port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        app_con(a, "gdbstub: socket 失败: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 1) != 0) {
        app_con(a, "gdbstub: bind/listen :%d 失败: %s", port, strerror(errno));
        close(lfd);
        return -1;
    }
    app_con(a, "gdbstub: 等待连接 127.0.0.1:%d (gdb: target remote :%d)", port, port);
    if (a->emit)
        fflush(stdout);
    int fd = accept(lfd, NULL, NULL);
    close(lfd);
    if (fd < 0)
        return -1;
    app_con(a, "gdbstub: 客户端已连接");

    char pkt[PKT_MAX], resp[PKT_MAX];
    int running = 1;
    while (running && !a->quit) {
        /* 读一帧: 忽略 +/- 与 Ctrl-C(0x03) */
        int c;
        do {
            char b;
            ssize_t r = recv(fd, &b, 1, 0);
            if (r <= 0) { close(fd); return 0; }
            c = (unsigned char)b;
        } while (c == '+' || c == '-' || c == 3);
        if (c != '$') continue;
        size_t n = 0;
        int bad = 0;
        for (;;) {
            char b;
            if (recv(fd, &b, 1, 0) != 1) { close(fd); return 0; }
            if (b == '#')
                break;
            if (n + 1 >= sizeof pkt) { bad = 1; continue; }
            pkt[n++] = b;
        }
        char cs[3] = { 0, 0, 0 };
        recv(fd, &cs[0], 1, 0);
        recv(fd, &cs[1], 1, 0);
        pkt[n] = 0;
        uint8_t want = (uint8_t)hex2u64(cs, NULL);
        char okc = (csum(pkt) == want && !bad) ? '+' : '-';
        send_all(fd, &okc, 1);
        if (okc == '-')
            continue;

        resp[0] = 0;
        const char *p = pkt;
        char cmd = *p++;

        switch (cmd) {
        case '?':
            snprintf(resp, sizeof resp, "T05");
            break;
        case 'g': {                                    /* 读全部寄存器 */
            size_t o = 0;
            for (int i = 0; i < 33; i++) {
                uint32_t r;
                reg_get(a, i, &r);
                for (int k = 0; k < 4; k++)
                    o += (size_t)snprintf(resp + o, sizeof resp - o, "%02x", (r >> (8 * k)) & 0xff);
            }
            break;
        }
        case 'G': {                                    /* 写全部寄存器 */
            for (int i = 0; i < 33; i++) {
                uint32_t r = 0;
                for (int k = 0; k < 4; k++)
                    r |= (uint32_t)hexval(p[i * 8 + k * 2]) << (8 * k + 4) |
                         (uint32_t)hexval(p[i * 8 + k * 2 + 1]) << (8 * k);
                reg_set(a, i, r);
            }
            snprintf(resp, sizeof resp, "OK");
            break;
        }
        case 'p': {                                    /* 读单寄存器 */
            int idx = (int)hex2u64(p, NULL);
            uint32_t v;
            reg_get(a, idx, &v);
            snprintf(resp, sizeof resp, "%02x%02x%02x%02x",
                     v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff);
            break;
        }
        case 'P': {                                    /* 写单寄存器 */
            const char *eq = strchr(p, '=');
            if (eq) {
                int idx = (int)hex2u64(p, NULL);
                uint32_t v = 0;
                for (int k = 0; k < 4; k++)
                    v |= (uint32_t)hexval(eq[1 + k * 2]) << (8 * k + 4) |
                         (uint32_t)hexval(eq[1 + k * 2 + 1]) << (8 * k);
                reg_set(a, idx, v);
                snprintf(resp, sizeof resp, "OK");
            }
            break;
        }
        case 'm': {                                    /* 读内存 */
            const char *e;
            uint64_t addr = hex2u64(p, &e);
            uint64_t len = 0;
            if (*e == ',')
                len = hex2u64(e + 1, NULL);
            if (!a->vm || len > 512) {
                snprintf(resp, sizeof resp, "E14");
                break;
            }
            uint8_t buf[512];
            if (rv_peek(a->vm, (uint32_t)addr, buf, (size_t)len) != 0) {
                snprintf(resp, sizeof resp, "E14");
                break;
            }
            size_t o = 0;
            for (uint64_t i = 0; i < len; i++)
                o += (size_t)snprintf(resp + o, sizeof resp - o, "%02x", buf[i]);
            break;
        }
        case 'M': {                                    /* 写内存 */
            const char *e, *c2;
            uint64_t addr = hex2u64(p, &e);
            uint64_t len = 0;
            if (*e == ',')
                len = hex2u64(e + 1, &c2);
            else
                c2 = e;
            if (*c2 != ':' || !a->vm || len > 512) {
                snprintf(resp, sizeof resp, "E14");
                break;
            }
            uint8_t buf[512];
            int badw = 0;
            for (uint64_t i = 0; i < len; i++) {
                int h1 = hexval(c2[1 + i * 2]), h2 = hexval(c2[2 + i * 2]);
                if (h1 < 0 || h2 < 0) { badw = 1; break; }
                buf[i] = (uint8_t)(h1 << 4 | h2);
            }
            if (!badw && rv_poke(a->vm, (uint32_t)addr, buf, (size_t)len) == 0)
                snprintf(resp, sizeof resp, "OK");
            else
                snprintf(resp, sizeof resp, "E14");
            break;
        }
        case 's': {                                    /* 单步 */
            if (!a->vm) { snprintf(resp, sizeof resp, "E01"); break; }
            int r = step_over_bp(a);
            stop_reply(resp, sizeof resp, r, a);
            break;
        }
        case 'c': {                                    /* 续跑(分块, 支持中断) */
            if (!a->vm) { snprintf(resp, sizeof resp, "E01"); break; }
            int r = 0;
            for (;;) {
                r = rv_run(a->vm, 8192);
                if (r != RV_MAXSTEPS)
                    break;                             /* 退出/断点/错误 */
                char probe;
                ssize_t got = recv(fd, &probe, 1, MSG_DONTWAIT);
                if (got == 1 && probe == 3) {          /* Ctrl-C */
                    r = RV_BREAK;
                    a->vm->hit_bp = a->vm->pc;
                    break;
                }
                if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    break;                             /* 连接异常 */
            }
            stop_reply(resp, sizeof resp, r, a);
            break;
        }
        case 'Z':
        case 'z': {                                    /* 断点: 仅 Z0/z0 */
            int type = (int)hex2u64(p, &p);
            if (*p == ',')
                p++;
            uint64_t addr = hex2u64(p, NULL);
            if (type != 0 || !a->vm) {
                resp[0] = 0;                           /* 其余类型不支持: 空回复 */
                break;
            }
            if (cmd == 'Z') {
                if (rv_bp_add(a->vm, addr) == 0)
                    snprintf(resp, sizeof resp, "OK");
                else
                    resp[0] = 0;
            } else {
                for (int i = 0; i < a->vm->n_bp; i++)
                    if (a->vm->bp[i] == addr) {
                        a->vm->bp[i] = a->vm->bp[--a->vm->n_bp];
                        break;
                    }
                snprintf(resp, sizeof resp, "OK");
            }
            break;
        }
        case 'H':                                      /* 线程选择: 接受 */
            snprintf(resp, sizeof resp, "OK");
            break;
        case 'q':
            if (strncmp(p, "Supported", 9) == 0)
                snprintf(resp, sizeof resp, "PacketSize=3ff");
            else if (strncmp(p, "Attached", 8) == 0)
                snprintf(resp, sizeof resp, "0");
            else if (strncmp(p, "fThreadInfo", 11) == 0 ||
                     strncmp(p, "sThreadInfo", 11) == 0)
                snprintf(resp, sizeof resp, "l");
            /* qTStatus/qRcmd/qXfer... → 空回复(不支持) */
            break;
        case 'k':
            running = 0;
            if (a->vm)
                a->vm->exited = 1;                     /* 视作杀掉 */
            continue;                                  /* 不回包 */
        case 'D':
            snprintf(resp, sizeof resp, "OK");
            send_pkt(fd, resp);
            running = 0;
            continue;
        default:
            break;                                     /* 未实现: 空回复 */
        }

        if (send_pkt(fd, resp) != 0)
            running = 0;
    }
    close(fd);
    app_con(a, "gdbstub: 会话结束");
    return 0;
}
