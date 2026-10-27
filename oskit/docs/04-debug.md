# 04 · 软件调试工具（`oskit debug`）

## 1. 目标与定位

实现一个**迷你符号级调试器**（对标"教学版 GDB"），让被调试程序在自己的控制下运行，
支持启动、软件断点、单步、寄存器/内存查看与修改、硬件观察点、栈回溯。
目标是把《操作系统》里"断点到底是怎么实现的"讲透：全部机制基于内核的
`ptrace(2)` 与 x86-64 调试寄存器，无任何 ptrace 之外的依赖。

宿主限制：仅支持 **x86-64 Linux**（寄存器布局与 INT3/DRn 机制都是 x86 语义）。

## 2. 功能清单

| 命令 | 缩写 | 说明 |
|------|------|------|
| `run [args...]` | `r` | 启动/重启被调试程序（在 exec 后陷入停下）|
| `continue` | `c` | 继续执行至下一断点/信号 |
| `step` | `s` | 单步一条指令（`PTRACE_SINGLESTEP`）|
| `break <addr\|symbol>` | `b` | 设软件断点：`b main`、`b *0x401100`、`b *0x+32`（相对加载基址）|
| `delete [n]` | `d` | 删除断点 |
| `info break` | | 断点列表 |
| `regs` | | 全量 GPR + rip/rflags（`PTRACE_GETREGS`）|
| `set $reg = value` | | 改寄存器（`PTRACE_SETREGS`）|
| `x/Nxg <addr>` / `x/Nxb` / `x/s` | | 查内存（十六进制 8/1 字节、字符串）|
| `watch <addr>` | | 硬件写观察点（DR0~DR3，最多 4 个）|
| `bt` | | 栈回溯（RBP 链 + 符号解析）|
| `maps` | | 打印 `/proc/<pid>/maps` |
| `signal <n>` | | 以信号 n 继续运行（默认 continue 吞掉信号）|
| `kill` / `quit` | `q` | 结束被调试进程 / 退出调试器 |

非目标：源码级单步/断点（需 DWARF 行号表）、多线程调试、反汇编（需要指令解码器，见 emu 的定位差异）。

## 3. 总体架构与生命周期

```
  oskit debug prog args
        │ fork
 ┌──────┴───────┐
 │ 子进程        │  PTRACE_TRACEME → execve(prog)
 └──────┬───────┘
 ┌──────▼───────────────────────────────────────┐
 │ 父进程（调试器主循环）                          │
 │  waitpid(exec 陷入) → 读 maps 计算加载基址      │
 │  REPL: 读命令 → 执行 → 等待下一个 stop 事件     │
 └──────────────────────────────────────────────┘
```

stop 事件分派（`waitpid` 返回后）：

1. `WIFEXITED/WIFSIGNALED` → 程序结束；
2. `WSTOPSIG == SIGTRAP`：
   - `rip-1` 命中软件断点 → 恢复原指令字节、rip 回退 1、停下待命令；
   - 处于"单步回填"流程 → 把断点 INT3 补回原位、停下；
   - DR6 命中 → 硬件观察点触发，报告地址并清 DR6；
   - 其余（exec 陷入、trap）→ 直接停下；
3. 其他信号（SIGSEGV 等）→ 打印信号与出错地址（`siginfo` 的 `si_addr`），默认不投递。

## 4. 关键实现原理

### 4.1 软件断点（INT3 机制）

```
设断点: PTRACE_PEEKTEXT 读原 8 字节 → 保存 → 最低字节改 0xCC → PTRACE_POKETEXT
命中:   SIGTRAP 停下, rip 指向 INT3 之后 → rip-1 定位断点
越过:   恢复原字节 → rip=断点地址 → PTRACE_SINGLESTEP 一步执行原指令
        → 停下后重新写回 INT3 → 等待用户 continue
```

### 4.2 硬件观察点（调试寄存器）

x86-64 有 DR0~DR3 四个地址寄存器与 DR7 控制寄存器：

```
DRn  = 监视地址（经 PTRACE_POKEUSER 写入 struct user.u_debugreg[n]）
DR7 |= 1<<(2n)        局部使能第 n 槽
DR7 |= 01b<<(16+4n)   RW=写(01)
DR7 |= 10b<<(18+4n)   LEN=8 字节(10)
触发: SIGTRAP 且 DR6 对应位置 1；读 DR6 判断槽位并清零
```

### 4.3 PIE 与符号解析

- 被调试文件先经 `elf.c` 解析符号表（`.symtab` 优先）；
- 若 `e_type == ET_DYN`（PIE），运行时地址 = 加载基址 + `st_value`；
  基址取自 `/proc/<pid>/maps` 中该可执行文件的**最低映射地址**；
- `bt` 沿 RBP 链取 `[rbp+8]` 的返回地址逐帧符号化（要求被调试程序
  `-fno-omit-frame-pointer` 编译，未保留帧指针时打印提示并尽早终止）。

### 4.4 内存与寄存器访问

- 寄存器：`PTRACE_GETREGS/SETREGS`（`struct user_regs_struct`）；
- 内存：`PTRACE_PEEKDATA/POKEDATA` 按字读写（`x/s` 逐字拼接到 NUL）；
  非法地址返回 EIO 时报告错误而非退出。

## 5. CLI 接口与示例会话

```
$ oskit debug ./tests/hello_frame
(osdk) break main
[1] main @ 0x55d1...2001164
(osdk) run
程序已启动, pid=4821 (在 exec 后停下)
(osdk) continue
命中断点 #1 main @ 0x...164
(osdk) bt
#0  0x...164  main
(osdk) step
... rip=0x...169  mov edi, 0x1  (仅打印地址)
(osdk) regs
rax 0x55d1...     rbx 0x7f8e...  ...
rip 0x...169  rflags 0x246  rsp 0x7ffd...
(osdk) x/2xg $rsp
0x7ffd...: 0x00000001  0x000055d1...
(osdk) quit
```

命令从标准输入逐行读取，因此可直接管道脚本化：
`printf 'b main\nr\nc\nbt\nq\n' | oskit debug ./prog`。

退出码：`0` 正常 quit；`1` 启动/访问失败；`2` 用法错误。

## 6. 验收标准

1. 对 `tests/hello_frame`（带 `-fno-omit-frame-pointer`）：`b main` → `run` →
   `continue` 能停在 main，`bt` 能给出 main 帧符号；
2. `x/s <指针>` 能读出被调试程序里的字符串常量；
3. `watch` 全局变量后程序写它即触发并停在被写语句之后；
4. 管道脚本模式全流程无人工输入完成；
5. 被 kill 后无僵尸进程（父进程显式 waitpid 回收）。

## 7. 已知限制与扩展方向

- 单步为指令级；源码级需补 DWARF 行号表（扩展方向：`info line`/`list`）；
- 不处理被调试程序 fork/clone 出的子进程（未设 `PTRACE_O_TRACEFORK`）；
- PIE 符号解析假设单次加载、无 ASLR 禁用需求（ASLR 开启同样正确，因基址取自 maps）；
- 扩展：`finish`（临时断点在返回地址）、条件断点、`next`（对 call 设临时断点）、
  attach 到已有进程（`PTRACE_ATTACH`）。
