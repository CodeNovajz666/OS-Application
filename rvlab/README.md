# RVLab —— RISC-V 系统开发工作台（TUI IDE，工作名可改）

一个跑在终端里的 RISC-V 系统级开发 IDE：**编辑 → 交叉构建 → 模拟运行 →
单步调试 → 指令级分析 → 安全检查 → 移植骨架**，一条链完成。
零第三方依赖的静态单二进制，SSH/WSL/低配机房机全能跑。

> 姊妹项目：[../oskit](../oskit)（六大命令行工具箱）。RVLab 复用其
> ELF 解析器、RV32 模拟器与安全检查规则，并新增 TUI / 编辑器 / GDB stub /
> 指令分析。设计论证见 [docs/rvlab.md](docs/rvlab.md) 第 1 节。

## 快速开始（WSL Ubuntu 内）

```bash
cd rvlab
bash build.sh        # 或 make；自动选择系统 gcc 或 ~/tc 免 root 工具链
./rvlab              # TUI 工作台（需要终端 ≥ 100×30）
./rvlab --demo 2     # 直接载入内置演示并进入 TUI
```

无终端会话时（脚本 / AI Agent / CI）：

```bash
printf 'demo 2\nrun\nstats\nquit\n' | ./rvlab     # headless 管道模式
./rvlab --script cmds.txt program.elf             # 脚本模式
```

GDB 互操作（装了 gdb-multiarch / riscv gdb 后）：

```bash
./rvlab --gdbstub 1234 --demo 2   # 窗口 1：模拟后端
gdb-multiarch -ex 'target remote :1234' -ex 'info registers' -ex si
```

## 界面与键位

```
 文件列表 │  编辑器(C/asm 高亮) │ 寄存器 x0-x31 + 内存
──────────┴─────────────────────┴─────────────────────
 控制台(运行输出/指令跟踪)  ·  :命令行  ·  keybar
```

`Tab` 切换面板 · 编辑器直接键入 · `:` 命令行 ·
`Ctrl-S` 保存 `Ctrl-B` 构建 `Ctrl-R` 运行 `Ctrl-N` 单步 `Ctrl-A` 分析 `Ctrl-K` 退出。
命令集（`：help` 全列表）：open/edit/w/build/skeleton/run/step/reset/bp/regs/mem/
disas/analyze/harden/stats/trace/gdb/quit，其中 analyze/harden/stats 支持 `--json`。

## 目录

```
src/   main · tui(面板/渲染) · buf(编辑缓冲) · cmd(命令中枢) · rv(RV32 虚拟机)
       elf(共享解析器) · analyze(指令组合/热点/加固) · buildtool · skeleton · gdbstub
tests/ rsp_client.c(纯 C 的 GDB-RSP 联测客户端) · run_tests.sh
docs/  rvlab.md 技术文档
```

## 已知限制

RV32I+M（无 A/C/S）；gdbstub 支持软断点与核心读写包；编辑器无 undo/补全。
路线图与设计权衡见技术文档。
