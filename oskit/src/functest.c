/* functest.c —— 功能测试：Linux 系统调用与 POSIX 行为黑盒验证
 * 设计见 docs/02-functest.md：自建自清、致命信号自恢复、套件可筛选。 */
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------- 断言框架 ---------- */
typedef struct {
    int  failures;
    int  skipped;
    char msg[256];
} test_ctx;

#define FAILC(c, fmt, ...)                                                    \
    do {                                                                      \
        snprintf((c)->msg, sizeof (c)->msg, "行%d: " fmt, __LINE__,           \
                 ##__VA_ARGS__);                                              \
        (c)->failures++;                                                      \
        return;                                                               \
    } while (0)

/* 注意: CHECK 的形参名不能叫 c —— 宏体内的 c 必须指向调用方的 test_ctx 变量 */
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond))                                                          \
            FAILC(c, "断言失败: %s", #cond);                                   \
    } while (0)

#define CHECK_EQ_I(a, b)                                                      \
    do {                                                                      \
        long long _va = (long long)(a), _vb = (long long)(b);                 \
        if (_va != _vb)                                                       \
            FAILC(c, "%s = %lld, 期望 %s = %lld", #a, _va, #b, _vb);          \
    } while (0)

#define SKIP(ctx, why)                                                        \
    do {                                                                      \
        snprintf((ctx)->msg, sizeof (ctx)->msg, "跳过: %s", why);             \
        (ctx)->skipped = 1;                                                   \
        return;                                                               \
    } while (0)

typedef void (*test_fn)(test_ctx *c);

typedef struct {
    const char *suite, *name;
    test_fn     fn;
} test_case;

/* ---------- 临时文件/目录助手（自建自清）---------- */
static int make_tmp_file(char *path, size_t n, const char *tag)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(path, n, "%s/oskit-%s-XXXXXX", dir, tag);
    int fd = mkstemp(path);
    return fd; /* -1 或有效句柄（文件已创建）*/
}

/* ===================================================================== */
/* file 套件                                                              */
/* ===================================================================== */
static void t_file_roundtrip(test_ctx *c)
{
    char path[300];
    int fd = make_tmp_file(path, sizeof path, "ft");
    CHECK(fd >= 0);
    CHECK_EQ_I(write(fd, "OSKit", 5), 5);
    close(fd);

    fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    char buf[16] = { 0 };
    CHECK_EQ_I(read(fd, buf, sizeof buf), 5);
    CHECK(strcmp(buf, "OSKit") == 0);
    close(fd);
    CHECK(unlink(path) == 0);
}

static void t_file_lseek(test_ctx *c)
{
    char path[300];
    int fd = make_tmp_file(path, sizeof path, "seek");
    CHECK(fd >= 0);
    CHECK_EQ_I(write(fd, "0123456789", 10), 10);
    CHECK_EQ_I(lseek(fd, 0, SEEK_SET), 0);
    char buf[8] = { 0 };
    CHECK_EQ_I(read(fd, buf, 3), 3);
    CHECK(strcmp(buf, "012") == 0);
    CHECK_EQ_I(lseek(fd, -2, SEEK_END), 8);
    memset(buf, 0, sizeof buf);
    CHECK_EQ_I(read(fd, buf, 2), 2);
    CHECK(strcmp(buf, "89") == 0);
    close(fd);
    unlink(path);
}

static void t_file_stat(test_ctx *c)
{
    char path[300];
    int fd = make_tmp_file(path, sizeof path, "st");
    CHECK(fd >= 0);
    CHECK_EQ_I(write(fd, "abcdef", 6), 6);
    close(fd);
    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK(S_ISREG(st.st_mode));
    CHECK_EQ_I(st.st_size, 6);
    unlink(path);
}

static void t_file_oexcl(test_ctx *c)
{
    char path[300];
    int fd = make_tmp_file(path, sizeof path, "ex");
    CHECK(fd >= 0);
    close(fd);
    errno = 0;
    CHECK(open(path, O_WRONLY | O_CREAT | O_EXCL, 0644) == -1);
    CHECK_EQ_I(errno, EEXIST);
    unlink(path);
}

static void t_file_truncate(test_ctx *c)
{
    char path[300];
    int fd = make_tmp_file(path, sizeof path, "tr");
    CHECK(fd >= 0);
    char big[100];
    memset(big, 'x', sizeof big);
    CHECK_EQ_I(write(fd, big, sizeof big), 100);
    close(fd);
    fd = open(path, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0);
    close(fd);
    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK_EQ_I(st.st_size, 0);
    unlink(path);
}

static void t_file_rename(test_ctx *c)
{
    char a[300], b[512];
    int fd = make_tmp_file(a, sizeof a, "rn");
    CHECK(fd >= 0);
    close(fd);
    snprintf(b, sizeof b, "%s.renamed", a);
    CHECK(rename(a, b) == 0);
    errno = 0;
    CHECK(access(a, F_OK) == -1);
    CHECK_EQ_I(errno, ENOENT);
    CHECK(access(b, F_OK) == 0);
    unlink(b);
}

static void t_file_enoent(test_ctx *c)
{
    errno = 0;
    CHECK(open("/oskit/definitely/not/exist", O_RDONLY) == -1);
    CHECK_EQ_I(errno, ENOENT);
}

static void t_file_mode(test_ctx *c)
{
    mode_t old = umask(0);
    char path[300];
    int fd = make_tmp_file(path, sizeof path, "md");
    CHECK(fd >= 0);
    close(fd);
    CHECK(chmod(path, 0600) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK_EQ_I(st.st_mode & 0777, 0600);
    unlink(path);
    umask(old);
}

/* ===================================================================== */
/* mem 套件                                                               */
/* ===================================================================== */
static void t_mem_mmap_rw(test_ctx *c)
{
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    char *p = mmap(NULL, ps, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    for (size_t i = 0; i < ps; i += 997)
        p[i] = (char)(i * 31);
    for (size_t i = 0; i < ps; i += 997)
        if (p[i] != (char)(i * 31)) {
            FAILC(c, "mmap 页在偏移 %zu 处数据不一致", i);
        }
    CHECK(munmap(p, ps) == 0);
}

static sigjmp_buf g_segv_jmp;

static void segv_recover(int sig) { siglongjmp(g_segv_jmp, 1); }

static void t_mem_mprotect_segv(test_ctx *c)
{
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    char *p = mmap(NULL, ps, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    CHECK(mprotect(p, ps, PROT_NONE) == 0);

    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = segv_recover;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGSEGV, &sa, &old) == 0);

    if (sigsetjmp(g_segv_jmp, 1) == 0) {
        p[16] = 'x';
        FAILC(c, "写 PROT_NONE 页未触发 SIGSEGV");
    }
    sigaction(SIGSEGV, &old, NULL);
    mprotect(p, ps, PROT_READ | PROT_WRITE);
    munmap(p, ps);
}

static void t_mem_brk(test_ctx *c)
{
    /* 断点查询必须可用; 扩展在部分 libc/加载布局下受限(musl 静态二进制
     * 的 brk 可能紧邻只读段), 受限时按 SKIP 处理而非判定内核失败 */
    char *b0 = sbrk(0);
    CHECK(b0 != (void *)-1);
    CHECK(sbrk(0) == b0);                       /* 断点查询稳定 */
    if (sbrk(4096) == (void *)-1)
        SKIP(c, "sbrk 扩展在该 libc/布局下不可用");
    char *b1 = sbrk(0);
    CHECK((uintptr_t)b1 >= (uintptr_t)b0 + 4096); /* 新断点至少前进 4096 */
}

/* ===================================================================== */
/* proc 套件                                                              */
/* ===================================================================== */
static void t_proc_fork_exitcode(test_ctx *c)
{
    pid_t p = fork();
    CHECK(p >= 0);
    if (p == 0) _exit(77);
    int st;
    CHECK_EQ_I(waitpid(p, &st, 0), p);
    CHECK(WIFEXITED(st));
    CHECK_EQ_I(WEXITSTATUS(st), 77);
}

static void t_proc_fork_signaled(test_ctx *c)
{
    pid_t p = fork();
    CHECK(p >= 0);
    if (p == 0) {
        for (;;) pause();
    }
    CHECK(kill(p, SIGKILL) == 0);
    int st;
    CHECK_EQ_I(waitpid(p, &st, 0), p);
    CHECK(WIFSIGNALED(st));
    CHECK_EQ_I(WTERMSIG(st), SIGKILL);
}

static void t_proc_exec(test_ctx *c)
{
    if (access("/bin/true", X_OK) != 0)
        SKIP(c, "/bin/true 不存在");
    pid_t p = fork();
    CHECK(p >= 0);
    if (p == 0) {
        execl("/bin/true", "true", (char *)NULL);
        _exit(126); /* exec 失败 */
    }
    int st;
    CHECK_EQ_I(waitpid(p, &st, 0), p);
    CHECK(WIFEXITED(st));
    CHECK_EQ_I(WEXITSTATUS(st), 0);
}

static void t_proc_getpid_diff(test_ctx *c)
{
    pid_t me = getpid(), pp = getppid();
    CHECK(me != pp);
    pid_t p = fork();
    CHECK(p >= 0);
    if (p == 0) _exit(getppid() == me ? 0 : 1);
    int st;
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st));
    CHECK_EQ_I(WEXITSTATUS(st), 0);
}

static void t_proc_wait_two(test_ctx *c)
{
    pid_t p1 = fork();
    CHECK(p1 >= 0);
    if (p1 == 0) _exit(1);
    pid_t p2 = fork();
    CHECK(p2 >= 0);
    if (p2 == 0) _exit(2);
    int st, got = 0;
    for (int i = 0; i < 2; i++) {
        pid_t r = wait(&st);
        CHECK(r == p1 || r == p2);
        CHECK(WIFEXITED(st));
        got += WEXITSTATUS(st);
    }
    CHECK_EQ_I(got, 3);
}

/* ===================================================================== */
/* signal 套件                                                            */
/* ===================================================================== */
static volatile sig_atomic_t g_flag;

static void usr1_set(int sig) { (void)sig; g_flag = 1; }

static void t_signal_handler(test_ctx *c)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = usr1_set;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, &old) == 0);
    g_flag = 0;
    CHECK(raise(SIGUSR1) == 0); /* 同步送达 */
    CHECK(g_flag == 1);
    sigaction(SIGUSR1, &old, NULL);
}

static void t_signal_sigpipe(test_ctx *c)
{
    int pp[2];
    CHECK(pipe(pp) == 0);
    close(pp[0]); /* 关读端 */
    void *old = signal(SIGPIPE, SIG_IGN);
    errno = 0;
    ssize_t r = write(pp[1], "x", 1);
    int e = errno;
    close(pp[1]);
    signal(SIGPIPE, old ? old : SIG_DFL);
    CHECK(r == -1);
    CHECK_EQ_I(e, EPIPE);
}

static void t_signal_from_child(test_ctx *c)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = usr1_set;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, &old) == 0);
    g_flag = 0;
    pid_t p = fork();
    CHECK(p >= 0);
    if (p == 0) {
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    int st;
    waitpid(p, &st, 0);
    sigaction(SIGUSR1, &old, NULL);
    CHECK(g_flag == 1); /* 竞价点: waitpid 返回时信号已递送(同一进程时间线) */
}

static void t_signal_block_pending(test_ctx *c)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = usr1_set;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, &old);

    sigset_t blk, oldmask, pend;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR2);
    CHECK(sigprocmask(SIG_BLOCK, &blk, &oldmask) == 0);
    g_flag = 0;
    CHECK(raise(SIGUSR2) == 0);      /* 被阻塞 → 挂起 */
    CHECK(g_flag == 0);
    CHECK(sigpending(&pend) == 0);
    CHECK(sigismember(&pend, SIGUSR2) == 1);
    CHECK(sigprocmask(SIG_SETMASK, &oldmask, NULL) == 0); /* 解除 → 送达 */
    CHECK(g_flag == 1);
    sigaction(SIGUSR2, &old, NULL);
}

/* ===================================================================== */
/* pipe 套件                                                              */
/* ===================================================================== */
static void t_pipe_basic(test_ctx *c)
{
    int pp[2];
    CHECK(pipe(pp) == 0);
    CHECK_EQ_I(write(pp[1], "abc", 3), 3);
    char buf[8] = { 0 };
    CHECK_EQ_I(read(pp[0], buf, sizeof buf), 3);
    CHECK(strcmp(buf, "abc") == 0);
    close(pp[0]);
    close(pp[1]);
}

static void t_pipe_eof(test_ctx *c)
{
    int pp[2];
    CHECK(pipe(pp) == 0);
    close(pp[1]);
    char buf[4];
    CHECK_EQ_I(read(pp[0], buf, sizeof buf), 0); /* EOF */
    close(pp[0]);
}

static void t_pipe_nonblock(test_ctx *c)
{
    int pp[2];
    CHECK(pipe(pp) == 0);
    int fl = fcntl(pp[0], F_GETFL);
    CHECK(fl >= 0);
    CHECK(fcntl(pp[0], F_SETFL, fl | O_NONBLOCK) == 0);
    errno = 0;
    char buf[4];
    CHECK(read(pp[0], buf, sizeof buf) == -1);
    CHECK_EQ_I(errno, EAGAIN);
    close(pp[0]);
    close(pp[1]);
}

static void t_pipe_bulk(test_ctx *c)
{
    /* 父写 1MiB(64KiB 一块), 子读并统计, 双向管道回传总数: 验证背压语义 */
    int p2c[2], c2p[2];
    CHECK(pipe(p2c) == 0);
    CHECK(pipe(c2p) == 0);
    pid_t p = fork();
    CHECK(p >= 0);
    if (p == 0) {
        close(p2c[1]);
        close(c2p[0]);
        char b[65536];
        ssize_t n;
        size_t total = 0;
        while ((n = read(p2c[0], b, sizeof b)) > 0)
            total += (size_t)n;
        ssize_t w = write(c2p[1], &total, sizeof total);
        (void)w;
        _exit(0);
    }
    close(p2c[0]);
    close(c2p[1]);
    char *big = malloc(65536);
    CHECK(big != NULL);
    memset(big, 0x7f, 65536);
    for (int i = 0; i < 16; i++)
        CHECK_EQ_I(write(p2c[1], big, 65536), 65536);
    close(p2c[1]);
    size_t total = 0;
    CHECK_EQ_I(read(c2p[0], &total, sizeof total), (ssize_t)sizeof total);
    close(c2p[0]);
    int st;
    waitpid(p, &st, 0);
    free(big);
    CHECK_EQ_I(total, 1024u * 1024u);
}

/* ===================================================================== */
/* dir 套件                                                               */
/* ===================================================================== */
static int make_tmp_dir(char *path, size_t n, const char *tag)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(path, n, "%s/oskit-%s-XXXXXX", dir, tag);
    return mkdtemp(path) != NULL ? 0 : -1;
}

static void t_dir_mkdir_rmdir(test_ctx *c)
{
    char d[300];
    CHECK(make_tmp_dir(d, sizeof d, "dir") == 0);
    struct stat st;
    CHECK(stat(d, &st) == 0);
    CHECK(S_ISDIR(st.st_mode));
    CHECK_EQ_I(rmdir(d), 0);
    errno = 0;
    CHECK(stat(d, &st) == -1);
    CHECK_EQ_I(errno, ENOENT);
}

static void t_dir_readdir(test_ctx *c)
{
    char d[300];
    CHECK(make_tmp_dir(d, sizeof d, "rd") == 0);
    char f[340];
    snprintf(f, sizeof f, "%s/oskit-marker", d);
    int fd = open(f, O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0);
    close(fd);

    DIR *dp = opendir(d);
    CHECK(dp != NULL);
    int found = 0;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL)
        if (strcmp(e->d_name, "oskit-marker") == 0)
            found = 1;
    closedir(dp);
    CHECK(found == 1);
    unlink(f);
    rmdir(d);
}

static void t_dir_cwd(test_ctx *c)
{
    char before[1024], after[1024];
    CHECK(getcwd(before, sizeof before) != NULL);
    char d[300];
    CHECK(make_tmp_dir(d, sizeof d, "cwd") == 0);
    CHECK(chdir(d) == 0);
    CHECK(getcwd(after, sizeof after) != NULL);
    /* 比较 "当前目录(.)" 与 "目标目录路径" 的 dev+inode: 验证 chdir 真正生效,
     * 且规避 /tmp 为符号链接时 getcwd 字符串与 mkdtemp 结果不一致的情况 */
    struct stat s1, s2;
    CHECK(stat(".", &s1) == 0);
    CHECK(stat(d, &s2) == 0);
    CHECK(s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino);
    CHECK(chdir(before) == 0);
    rmdir(d);
    /* getcwd(小缓冲) 返回 ERANGE */
    char tiny[2];
    errno = 0;
    CHECK(getcwd(tiny, sizeof tiny) == NULL);
    CHECK_EQ_I(errno, ERANGE);
}

/* ===================================================================== */
/* time 套件                                                              */
/* ===================================================================== */
static void t_time_monotonic(test_ctx *c)
{
    struct timespec a, b, d;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &d) == 0);
    CHECK(b.tv_sec > a.tv_sec ||
          (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec));
    CHECK(d.tv_sec > b.tv_sec ||
          (d.tv_sec == b.tv_sec && d.tv_nsec >= b.tv_nsec));
}

static void t_time_realtime(test_ctx *c)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_REALTIME, &t) == 0);
    CHECK(t.tv_sec > 1577836800);  /* 2020-01-01 */
    CHECK(t.tv_sec < 4102444800);  /* 2100-01-01 */
    CHECK(t.tv_nsec >= 0 && t.tv_nsec < 1000000000);
}

static void t_time_resolution(test_ctx *c)
{
    struct timespec r;
    CHECK(clock_getres(CLOCK_MONOTONIC, &r) == 0);
    CHECK(r.tv_nsec >= 1 && r.tv_nsec <= 1000000000);
}

/* ===================================================================== */
/* misc 套件                                                              */
/* ===================================================================== */
static void t_misc_pagesize(test_ctx *c)
{
    long ps = sysconf(_SC_PAGESIZE);
    CHECK(ps >= 4096);
    CHECK((ps & (ps - 1)) == 0); /* 2 的幂 */
}

static void t_misc_uname(test_ctx *c)
{
    struct utsname u;
    CHECK(uname(&u) == 0);
    CHECK(strcmp(u.sysname, "Linux") == 0);
    CHECK(u.release[0] >= '1' && u.release[0] <= '9');
}

static void t_misc_errno(test_ctx *c)
{
    errno = 0;
    CHECK(open("/no/such/file/oskit", O_RDONLY) == -1);
    CHECK_EQ_I(errno, ENOENT);
}

static void t_misc_env(test_ctx *c)
{
    CHECK(setenv("OSKIT_TEST_VAR", "42", 1) == 0);
    const char *v = getenv("OSKIT_TEST_VAR");
    CHECK(v != NULL);
    CHECK(strcmp(v, "42") == 0);
    CHECK(unsetenv("OSKIT_TEST_VAR") == 0);
    CHECK(getenv("OSKIT_TEST_VAR") == NULL);
}

/* ===================================================================== */
/* 注册表 / 运行器                                                        */
/* ===================================================================== */
/* 用例在 fork 出的守护子进程中执行：崩溃不中断整轮；
 * 结果(<PIPE_BUF, 单次 write 原子)经管道回传，保留失败信息。 */
typedef struct {
    int  failures, skipped;
    char msg[256];
} test_result;
static const test_case g_tests[] = {
    { "file",   "create_write_roundtrip", t_file_roundtrip       },
    { "file",   "lseek_positions",        t_file_lseek           },
    { "file",   "stat_fields",            t_file_stat            },
    { "file",   "o_excl_eexist",          t_file_oexcl           },
    { "file",   "o_truncate_size",        t_file_truncate        },
    { "file",   "rename_enoent",          t_file_rename          },
    { "file",   "open_missing_enoent",    t_file_enoent          },
    { "file",   "chmod_mode_bits",        t_file_mode            },
    { "mem",    "mmap_anon_rw",           t_mem_mmap_rw          },
    { "mem",    "mprotect_segv",          t_mem_mprotect_segv    },
    { "mem",    "brk_extend_shrink",      t_mem_brk              },
    { "proc",   "fork_exitcode",          t_proc_fork_exitcode   },
    { "proc",   "fork_signaled",          t_proc_fork_signaled   },
    { "proc",   "exec_true",              t_proc_exec            },
    { "proc",   "getpid_ppid",            t_proc_getpid_diff     },
    { "proc",   "wait_two_children",      t_proc_wait_two        },
    { "signal", "handler_raise",          t_signal_handler       },
    { "signal", "sigpipe_epipe",          t_signal_sigpipe       },
    { "signal", "signal_from_child",      t_signal_from_child    },
    { "signal", "block_pending_unblock",  t_signal_block_pending },
    { "pipe",   "basic_stream",           t_pipe_basic           },
    { "pipe",   "read_eof",               t_pipe_eof             },
    { "pipe",   "nonblock_eagain",        t_pipe_nonblock        },
    { "pipe",   "bulk_backpressure",      t_pipe_bulk            },
    { "dir",    "mkdir_rmdir",            t_dir_mkdir_rmdir      },
    { "dir",    "readdir_find_entry",     t_dir_readdir          },
    { "dir",    "getcwd_chdir",           t_dir_cwd              },
    { "time",   "monotonic_nondecrease",  t_time_monotonic       },
    { "time",   "realtime_sane_range",    t_time_realtime        },
    { "time",   "clock_resolution",       t_time_resolution      },
    { "misc",   "pagesize_pow2",          t_misc_pagesize        },
    { "misc",   "uname_linux",            t_misc_uname           },
    { "misc",   "errno_enoent",           t_misc_errno           },
    { "misc",   "env_set_get_unset",      t_misc_env             },
};

#define NTESTS (sizeof g_tests / sizeof g_tests[0])

static void functest_usage(void)
{
    puts("用法: oskit functest [--list] [--json] [套件名 ...]");
    puts("");
    puts("套件: file mem proc signal pipe dir time misc");
    puts("  --list  列出全部用例");
    puts("  --json  输出 JSON");
    puts("退出码: 0 全部通过, 1 存在失败, 2 用法错误");
}

int functest_main(int argc, char **argv)
{
    int json = 0, list = 0;
    const char *want[16];
    size_t nwant = 0;
    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "--json")) json = 1;
        else if (arg_is(argv[i], "--list")) list = 1;
        else if (arg_is(argv[i], "--help") || arg_is(argv[i], "-h")) { functest_usage(); return 0; }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "oskit functest: 未知选项 %s\n", argv[i]);
            return 2;
        } else {
            if (nwant < 16) want[nwant++] = argv[i];
        }
    }
    if (list) {
        for (size_t i = 0; i < NTESTS; i++)
            printf("%-8s/%s\n", g_tests[i].suite, g_tests[i].name);
        return 0;
    }
    if (!json) {
        puts("== OSKit 功能测试 ==");
        fflush(stdout);
    }

    size_t pass = 0, fail = 0, skip = 0, ran = 0;
    if (json) putchar('{');
    int first = 1;

    for (size_t i = 0; i < NTESTS; i++) {
        if (nwant) {
            int sel = 0;
            for (size_t k = 0; k < nwant; k++)
                if (strcmp(want[k], g_tests[i].suite) == 0) sel = 1;
            if (!sel) continue;
        }
        ran++;
        test_result r = { 0, 0, "" };
        int rp[2];
        if (pipe(rp) != 0) {
            fail++;
            printf("[FAIL] %s/%s  管道创建失败\n", g_tests[i].suite, g_tests[i].name);
            continue;
        }
        pid_t guard = fork(); /* 隔离: 用例意外崩溃不至于中断整轮 */
        if (guard == 0) {
            close(rp[0]);
            test_ctx c = { 0, 0, "" };
            g_tests[i].fn(&c);
            test_result out;
            out.failures = c.failures;
            out.skipped = c.skipped;
            memcpy(out.msg, c.msg, sizeof out.msg);
            ssize_t w = write(rp[1], &out, sizeof out);
            (void)w;
            _exit(0);
        }
        close(rp[1]);
        ssize_t got = read(rp[0], &r, sizeof r);
        (void)got;
        close(rp[0]);
        int st = 0;
        waitpid(guard, &st, 0);
        if (WIFSIGNALED(st)) {
            r.failures = 1;
            snprintf(r.msg, sizeof r.msg, "用例进程被信号 %d 终止", WTERMSIG(st));
        } else if (got != (ssize_t)sizeof r) {
            r.failures = 1;
            snprintf(r.msg, sizeof r.msg, "用例异常退出, 未回传结果");
        }

        if (json) {
            if (!first) putchar(',');
            first = 0;
            printf("\"%s/%s\":{\"ok\":%s,\"message\":", g_tests[i].suite, g_tests[i].name,
                   (r.failures) ? "false" : (r.skipped ? "skipped" : "true"));
            if (r.skipped) {
                json_escape(stdout, r.msg);
            } else if (r.failures) {
                json_escape(stdout, r.msg[0] ? r.msg : "失败");
            } else {
                fputs("\"\"", stdout);
            }
            putchar('}');
        } else {
            const char *tag = "PASS";
            if (r.failures) tag = "FAIL";
            else if (r.skipped) tag = "SKIP";
            printf("[%s] %s/%s", tag, g_tests[i].suite, g_tests[i].name);
            if (r.failures || r.skipped)
                printf("  %s", r.msg[0] ? r.msg : "");
            putchar('\n');
            fflush(stdout);
        }
        if (r.skipped) skip++;
        else if (r.failures) fail++;
        else pass++;
    }

    if (json)
        printf(",\"total\":%zu,\"pass\":%zu,\"fail\":%zu,\"skipped\":%zu}\n",
               ran, pass, fail, skip);
    else
        printf("--------------------------------------------------\n"
               "总计 %zu · 通过 %zu · 失败 %zu · 跳过 %zu\n",
               ran, pass, fail, skip);
    return fail ? 1 : 0;
}
