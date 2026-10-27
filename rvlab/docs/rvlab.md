# RVLab —— RISC-V 系统开发工作台（TUI IDE）技术文档

> 工作名 RVLab · 运行于 Linux（WSL2/Ubuntu 验证）· C11 · 零第三方依赖
> 上游姊妹项目：`../oskit`（六大命令行工具，RVLab 复用其核心模块）

## 1. 定位与设计论证（为什么这样做）

**目标**：面向 RISC-V 系统级开发（裸机内核、教学实验、芯片前期验证）的
一体化工作台，把"**编辑 → 交叉构建 → 模拟运行 → 单步调试 → 指令级分析 →
安全检查 → 移植骨架**"压缩进一个终端窗口。

**三条设计立场（即对"再做一个 GUI IDE"的反驳）**：

1. **TUI 而非 GUI/Electron**：
   - 使用场景是 SSH / WSL / 云开发容器 / 高校机房低配机——TUI 全覆盖，
     静态单二进制（<300KB）拷贝即用，内存占用以 MB 计；
   - VS Code + 插件生态已存在，再造 GUI 是 0-to-N；而"系统级 RISC-V
     开发链路一体化工作台"尚无对应物，这才是 0-to-1；
   - 终端形态与 git/helm/lazygit 一脉相承，学习成本低。
2. **AI-Agent 原生（2026 年的真热点）**：所有功能都有命令行入口
   （`--script` / 管道 stdin），分析类命令输出 `--json`，使 AI 编码代理与
   CI 可以无界面驱动整套链路——IDE 不止给人用，也给 Agent 用。
3. **与标准生态互操作而非自成孤岛**：内置 GDB 远程串行协议服务
   （gdbstub, RFC 无但有事实标准），`target remote` 即可让 gdb /
   gdb-multiarch / VS Code(cortex-debug 等) / 任意支持 GDB RSP 的前端
   连到 RVLab 的内置模拟器上调试——RVLab 既是 IDE，也是生态的模拟后端。

**语言选 C**：与 oskit 共享已验证的 ELF 解析器 / RV32 解释器 / 加固规则
（复用即质量），免 root 的 musl 工具链可立即构建，静态链接随处部署。

## 2. 功能总览

| 模块 | 功能 | 对应 oskit 血缘 |
|------|------|----------------|
| 工作台(TUI) | 文件面板 / 编辑器 / 代码高亮查看 / 寄存器·内存·控制台面板 / 命令行(`:`) | — |
| 编辑器 | 打开·编辑·保存，脏标记，行号，C/汇编高亮 | — |
| 构建管线 | 自动探测 RISC-V 交叉工具链，单文件/工程构建，错误行解析跳转 | port/check 思路 |
| 模拟运行 | 内置 RV32I+M 虚拟机（UART/Exit/mtime MMIO，与 QEMU virt 地址一致）| emu 主体 |
| 调试 | 断点/单步/运行/重置，寄存器与内存实时视图，指令跟踪 | emu + debug 交互 |
| 指令级分析 | 指令组合直方图、分支命中率、访存计数、热点符号 Top-N（`--json`）| emu+elf |
| 安全检查 | NX/PIE/RELRO/Canary/FORTIFY + 危险符号（RISC-V ELF 同样适用）| vuln 规则 |
| 移植骨架 | riscv32/riscv64 裸机工程一键生成（crt0/linker/驱动/Makefile）| port/gen 模板 |
| GDB stub | RSP 协议：g/G/m/M/p/P/s/c/Z0/z0/?/qSupported/k/D，TCP 单连接 | 全新 |

## 3. 总体架构

```
                    ┌────────────────────────────────────┐
   终端 stdin ──────▶│           main.c                  │
   --script / 管道  │  isatty? ──┬─ TUI(tui.c) ──┐      │
                    │            └─ headless ────┤      │
                    │                  ▼         ▼      │
                    │           cmd.c 命令解释器        │◀─ AI Agent / CI
                    │  (run/step/bp/analyze/build/...)  │
                    └───┬──────────┬─────────┬──────────┘
                        ▼          ▼         ▼
                  ┌────────┐ ┌─────────┐ ┌──────────┐
                  │ rv.c   │ │analyze.c│ │buildtool │
                  │ RV32VM │ │+skeleton│ │+gdbstub  │
                  └───┬────┘ └────┬────┘ └────┬─────┘
                      ▼           ▼           ▼
                  ┌───────────────────────────────┐
                  │ elf.c(共享解析器)  buf.c(编辑缓冲)│
                  └───────────────────────────────┘
```

- **cmd.c 是系统的中枢**：TUI 的 `:` 命令行、headless 脚本、管道 stdin
  走同一解释器 → 同一套命令即可人用也可被 Agent 用（设计立场 2 的落点）；
- **rv.c 是库化的虚拟机**：相对 oskit emu 的改造——取消全局状态与 `die()`，
  改为句柄 `rv_vm` + 返回码 + 错误消息缓冲；增加断点表、每类指令计数、
  符号级热点归因（有序符号表 + 二分查找）、UART/跟踪回调注入。

## 4. 关键设计细节

### 4.1 虚拟机(rv.c)

```c
rv_vm *rv_create(size_t ram_mib);
int rv_load_elf(rv_vm*, buf, len);       /* ELF32 · EM_RISCV · PT_LOAD 装载 */
int rv_load_flat(rv_vm*, buf, len, base);
int rv_step(rv_vm*);   /* 0=已执行 1=程序退出 2=断点停(未执行) 3=错误 */
int rv_run(rv_vm*, uint64_t max);        /* 批量执行至 退出/断点/上限 */
int rv_bp_add(rv_vm*, uint64_t addr);
void rv_disasm(uint32_t inst, char *out, size_t n);  /* 轻量反汇编 */
```

- 断点语义：`rv_run` 在**取指前**检查 pc，命中即停且不执行；调用方
  需先 `rv_step` 一次越过断点再续跑（TUI 与 gdbstub 共同约定，防止
  oskit debug 里记录过的"停在断点死循环"问题在更高层复发）；
- 统计内建：`op_count[12]`（按 opcode 类 + M 扩展单列）、分支总数/命中数、
  load/store 计数——分析面板与 `:stats` 零成本取数；
- 热点符号：加载 ELF 后对 symtab 按 addr 排序，执行时二分定位
  （log₂(符号数) 每条指令，全速运行可承受）。

### 4.2 GDB 远程协议服务(gdbstub.c)

- 传输：TCP `--gdbstub <port>`，单连接；帧格式 `$<data>#<cs>`，应答 `+`；
- 寄存器序（'g'/'G'）：x0..x31 + pc，每寄存器 8 个十六进制、目标端小端；
- 停止应答：单步/断点 `T05`，程序退出 `W<exit>`；
- 断点 `Z0/z0` 直接映射 `rv_bp_add/clear`；
- 与 TUI 互斥使用（同一虚拟机实例），`--gdbstub` 模式下不进 TUI。

### 4.3 命令解释器(cmd.c)

```
open <文件>        demo [1|2]        edit <文件>      w(保存)
build [文件]       skeleton <目标> <名字>             ls
run [最大指令数]   step [n]          reset            bp <符号|0x地址>
bpdel [n|all]      regs              mem <地址>       disas [n]
analyze [--json]   harden [--json]   stats [--json]   trace [on|off]
gdb <端口>         help              quit
```

- headless（非 tty / `--script`）下输出直写 stdout；TUI 下写入控制台面板；
- `--json` 的三条命令面向 Agent/CI，输出结构见源码头注释。

### 4.4 TUI(tui.c + buf.c)

- 原语：termios 原始模式 + ANSI 转义（备用屏/光标/256 色/UTF-8 边框），
  自绘不依赖 ncurses（保持静态单二进制）；
- 布局：

```
┌ RVLab —— 文件名 ─ 状态(运行/停止/退出码) ─ 指令数 ─────────┐
│ 文件列表(22) │  编辑器/查看器(语法高亮+行号)  │ 寄存器 x0-x31 │
│              │                              │ 内存十六进制   │
├──────────────┴──────────────────────────────┴───────────────┤
│ 控制台(输出/跟踪)                                            │
├─────────────────────────────────────────────────────────────┤
│ :命令行                                    keybar 快捷键提示  │
└─────────────────────────────────────────────────────────────┘
```

- 焦点循环 Tab/Shift-Tab；编辑器内直接键入；全局键：
  `Ctrl-S` 保存 · `Ctrl-B` 构建 · `Ctrl-R` 运行 · `Ctrl-N` 单步 ·
  `Ctrl-A` 分析 · `Ctrl-K` 退出 · `:` 命令行；
- 跟踪(trace on)时每条执行指令以 `pc: 反汇编` 追加到控制台（环形缓冲）。

### 4.5 编辑器(buf.c)

- 行数组缓冲：插入/删除字符、拆行/并行、光标与滚动、保存（原地写回）；
- 语法高亮(view 渲染)：C 关键字/字符串/注释/预处理；汇编助记符/寄存器/
  标号/注释；逐行着色（无跨行状态，保证渲染 O(行数)）；
- 无撤销/无多窗口（文档明示，教学场景够用且保持代码可读）。

### 4.6 构建与骨架

- 工具链探测序：`$RISCV_CC` → `riscv64-unknown-elf-gcc` →
  `riscv64-linux-gnu-gcc` → `riscv32-unknown-elf-gcc` → `riscv-none-elf-gcc`；
- 单文件构建：`-march=rv32im -mabi=ilp32 -nostdlib -static
  -Wl,-Ttext=0x80000000`；错误按 `文件:行:列: 级别: 消息` 解析进问题列表，
  编辑器可跳转；
- 骨架：riscv32/riscv64 两目标，产物 crt0.S / linker.ld / kernel.c /
  Makefile / run.sh（模板来自 oskit port，已实测）。

## 5. 交付形态与测试

```
rvlab/
├── README.md  docs/rvlab.md  Makefile  build.sh
├── src/ (tui/cmd/rv/elf/analyze/buf/buildtool/skeleton/gdbstub/main)
└── tests/ (rsp_client.c —— 纯 C 的 RSP 联测客户端, run_tests.sh)
```

验收标准：
1. `rvlab --script` 与管道模式：demo2 运行输出 `fib(10)=55`、退出码 55；
2. `analyze --json`/`harden --json`/`stats --json` 可被 `json.tool` 级校验；
3. rsp_client 走通 `qSupported→m 读内存→s 单步→g 读 pc→z0 断点→c 命中→k`；
4. TUI 在 100×30 终端下无残影渲染（人工验收项）；
5. 无工具链时构建命令给出明确指引而不是失败堆栈。

## 6. 已知限制与路线图

- VM 为 RV32I+M（无 A/C/S 模式），运行 64 位程序会明确报错（路线图：C 扩展→S 模式）；
- gdbstub 支持软断点 Z0 与核心读写包，未实现 watchpoint(Z2)与 vCont；
- 编辑器无 undo、无补全（路线图：LSP 式外部补全挂接）；
- TUI 未支持鼠标（保持按键模型简单）。
