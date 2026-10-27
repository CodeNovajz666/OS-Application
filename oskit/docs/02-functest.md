# 02 · 功能测试工具（`oskit functest`）

## 1. 目标与定位

以**黑盒功能测试**验证宿主 Linux 内核对 POSIX/系统调用语义的实现正确性：
针对文件、内存、进程、信号、管道、目录、时间等子系统给出
"预期行为断言"，逐条判定 PASS/FAIL。用途：

- 操作系统课程的"内核行为观察"实验底座；
- 交叉环境（不同内核版本/容器/虚拟机）行为差异排查；
- 作为 `oskit` 自身的回归测试（工具所依赖的内核能力先自证）。

它不是完整 POSIX 认证套件（那是 LTP/PCTS 的规模），而是**精选 8 组、约 30 个用例**
的可读实现——每个用例都是一段独立、可抄进作业里的小实验。

## 2. 功能范围（测试套件）

| 套件 | 覆盖点 |
|------|--------|
| `file` | open(O_CREAT/O_TRUNC/O_EXCL)、write/read 往返、lseek、fstat 字段、rename、unlink、错误码 ENOENT |
| `mem` | mmap 匿名读写、munmap、mprotect(PROT_READ 写触发 SIGSEGV)、brk 基本扩展 |
| `proc` | fork 子进程退出码回收（WIFEXITED/WEXITSTATUS）、execve(/bin/true)、getpid 父子不同 |
| `signal` | sigaction 注册句柄 + kill 自身送达、SIGPIPE 语义（忽略后 write 返回 EPIPE）|
| `pipe` | 匿名管道字节流、读端 EOF、非阻塞 EAGAIN |
| `dir` | mkdir/rmdir、opendir/readdir 找到自建项、getcwd/setcwd 往返 |
| `time` | CLOCK_MONOTONIC 单调非降、CLOCK_REALTIME 合理范围、timespec 精度 |
| `misc` | getpagesize、uname 语义、errno 机制（open 缺失文件置 ENOENT）|

## 3. 架构与断言框架

```
        functest.c
 ┌─────────────────────────────┐
 │ 用例注册表 g_tests[]          │ {suite, name, fn(test_ctx*)}
 ├─────────────────────────────┤
 │ test_ctx: 失败计数/首错信息   │
 │  CHECK(cond)      → 失败记 1  │
 │  CHECK_EQ_I(a,b)  → 整型比对  │
 │  CHECK_EQ_S(a,b)  → 字符串    │
 │  CHECK_SYS(cond)  → 失败附 errno 说明 │
 └──────────────┬──────────────┘
                ▼
  逐用例: 打印 [PASS]/[FAIL] … → 汇总(通过率) → 退出码
```

- 用例内部**自建自清**：临时文件/目录放 `TMPDIR` 下带 `oskit-` 前缀，结束即删；
  涉及子进程的用例负责 `waitpid`，不留僵尸；
- 涉及致命信号（SIGSEGV 预期路径）的用例使用 `sigsetjmp/siglongjmp` 自恢复，
  保证单个用例崩溃不影响整轮执行；
- 每用例限时由负载本身保证（无死循环结构；管道用例全部非阻塞或定长）。

## 4. 关键用例的原理说明

1. **mprotect 触发 SIGSEGV**：mmap 两页 → mprotect 首页 PROT_NONE → 安装恢复型
   SIGSEGV 句柄 → 读该页。句柄内 `siglongjmp` 回用例主体，"能到达恢复点"即证明
   内核按 PROT_NONE 拒绝了访问；句柄恢复原样。
2. **SIGPIPE 语义**：`signal(SIGPIPE, SIG_IGN)` 后写已关闭读端的管道，
   期望 `write == -1 && errno == EPIPE`（而非进程被杀），同时验证"忽略信号后
   以错误码替代默认行为"这一 POSIX 规则。
3. **execve 成功即失忆**：子进程 exec `/bin/true`，exec 成功后原代码不再执行，
   因此断言只能放在父进程：`waitpid` 得到退出码 0 且 `WIFEXITED`。
4. **CLOCK_MONOTONIC 单调性**：连读 3 次，断言 `t3 >= t2 >= t1`（同秒边界也不回退）。

## 5. CLI 接口

```
oskit functest [--list] [--json] [套件名 ...]
```

| 参数 | 说明 |
|------|------|
| （无套件名）| 运行全部 8 套 |
| `--list` | 列出套件与用例数 |
| `--json` | `{total, pass, fail, skipped, cases:[{suite,name,ok,message}]}` |

退出码：`0` 全部通过；`1` 存在失败用例；`2` 用法错误。
（刻意让失败退出码非零，便于放进 `make test` 与 CI。）

## 6. 输出示例

```
== OSKit 功能测试 ==
[PASS] file/open_close
[PASS] file/read_write_roundtrip
[FAIL] pipe/nonblock_eagain : 第32行: 期望 EAGAIN, 实际 11(继续读出数据)
...
--------------------------------------------------
总计 31 · 通过 30 · 失败 1   (pipe 套件)
```

## 7. 验收标准

1. 在正常 Ubuntu 22.04/WSL2 上全量通过（失败即内核或环境异常，值得报告）；
2. 人为制造失败（如只读挂载下建文件）能打印明确定位信息而非崩溃；
3. 连跑两轮无残留：`ls /tmp/oskit-*` 为空、`ps` 无遗留子进程；
4. `--json` 与文本模式结论一致。

## 8. 已知限制与扩展方向

- WSL2 的 `/init` 与 9P（`/mnt/c`）语义与原生 ext4 有差异，套件一律在 `TMPDIR`
  （默认 `/tmp`，ext4）执行，规避 9P 特殊行为；如需验证 9P 可 `TMPDIR=/mnt/c/... oskit functest`；
- 未覆盖：信号队列实时语义（sigqueue）、fcntl 记录锁、epoll/inotify、cgroup —— 均留作扩展套件；
- 并发压力与故障注入（模拟 ENOSPC）是后续方向，可复用本框架直接加用例。
