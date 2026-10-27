# 00 · 总体架构与技术选型

> OSKit（工作名，名称待定）· 技术文档总览
> 目标环境：WSL2 / Ubuntu 22.04 (x86-64) · 开发语言：C11 · 构建系统：GNU Make

## 1. 项目定位

面向开源操作系统（Linux 及 RISC-V/ARM 裸机系统）的**开发、测试与教学**场景，
提供一套可独立编译、无第三方依赖的命令行工具箱，覆盖操作系统工程实践的六个核心环节：

```
 ┌──────────────────────────────────────────────────────────────┐
 │                        oskit <子命令>                          │
 ├──────────┬──────────┬──────────┬──────────┬──────────┬────────┤
 │   perf   │functest  │   vuln   │  debug   │   emu    │  port  │
 │ 性能测试  │ 功能测试  │ 漏洞分析  │ 软件调试  │ 系统模拟器│ 系统移植│
 └────┬─────┴────┬─────┴────┬─────┴────┬─────┴────┬─────┴───┬────┘
      │          │          │          │          │         │
   微基准     系统调用     ELF 静态    ptrace     RV32 指令   工程骨架
   CPU/内存   POSIX 行为   安全加固    INT3/DRn   集模拟     交叉编译
   磁盘/调度   黑盒测试     危险函数    断点单步   MMIO 设备  QEMU 集成
```

六个工具覆盖了"操作系统实验/内核开发"课程的典型链路：
**移植启动（port）→ 模拟运行（emu）→ 功能验证（functest）→ 性能度量（perf）→
安全加固（vuln）→ 故障定位（debug）**。

## 2. 技术选型与理由

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 语言 | C11 | 与操作系统同一层级的系统语言；直接使用 Linux API；教学可读性好 |
| 平台 API | Linux 系统调用 + POSIX | 工具本身就是"基于开源操作系统"的实践；WSL2 提供完整 Linux 内核 |
| 依赖 | 仅 libc + libpthread | `gcc + make` 即可构建；无包管理负担，可复制到任何 Linux |
| 形态 | 单二进制 + 子命令分发 | 仿 git/docker 交互习惯；分发与脚本化成本最低 |
| 输出 | 人读表格 + `--json` 机器格式 | 兼顾课堂演示与 CI 集成 |
| 模拟器 ISA | RISC-V RV32I+M | 开放指令集、编码规整、无商业授权问题；教学界事实标准 |
| 调试器技术 | ptrace + INT3 + x86 调试寄存器 | 与 GDB 同源的内核机制，展示调试器底层原理 |

## 3. 源码架构

```
src/
├── main.c        入口：子命令表分发、全局帮助、版本信息
├── common.c/h    计时(clock_gettime)、样本统计(分位数)、JSON 转义、
│                 整体读文件、错误报告 —— 全部工具复用
├── elf.c/h       共享 ELF 解析器：ELF32/ELF64、程序头/节头/符号表、
│                 vaddr→offset 换算 —— 被 vuln / debug / emu 三者复用
├── perf.c        [依赖 common] 11 项微基准 + 统计汇总
├── functest.c    [依赖 common] 8 组约 30 个系统调用功能用例 + 断言框架
├── vuln.c        [依赖 elf]    安全加固检查 + 危险符号扫描 + 风险报告
├── debug.c       [依赖 elf]    ptrace 调试器（x86-64 专用）
├── emu.c         [依赖 elf]    RV32 解释执行循环 + MMIO 设备模型
└── port.c        [依赖 common] 环境体检 / 骨架生成 / QEMU 启动 / 镜像打包
```

**模块间唯一共享的重逻辑是 ELF 解析**（vuln 需要段与符号、debug 需要符号表、
emu 需要可加载段），抽取为 `elf.c` 避免三份实现漂移；其余工具彼此独立，
单文件即单工具，便于按文档逐个阅读。

## 4. 公共模块约定

### 4.1 common（`src/common.h`）

| 接口 | 用途 |
|------|------|
| `now_ns()` | `CLOCK_MONOTONIC` 纳秒级计时，所有基准测试的时钟源 |
| `samples` / `stats_compute()` | 样本收集与统计：min / median / mean / p95 / max |
| `xread_file()` | 一次性读入文件（ELF 解析前奏）|
| `json_escape()` | 输出 JSON 字符串时转义控制字符与引号 |
| `die()` / `warnf()` | 统一错误报告格式 `<prog>: <file>: <msg>` |

### 4.2 elf（`src/elf.h`）

统一的只读 ELF 视图：

```c
elf_file *elf_open_buf(void *buf, size_t len);   /* 解析头/段/节，做全部边界检查 */
int  elf_phdrs(ef, &n);                          /* 程序头数组 */
int  elf_shdrs(ef, &n);                          /* 节头数组 + 节名(shstrtab) */
int  elf_sym_lookup(ef, name, &addr, &size);     /* symtab→dynsym 顺序查符号 */
int  elf_vaddr_to_off(ef, vaddr, &off);          /* 经 PT_LOAD 换算文件偏移 */
void elf_for_each_sym(ef, cb(userdata, name, addr, size, bind, type));
```

支持 ELF32/ELF64、小端宿主读取小端文件（x86-64 宿主成立；大端目标留作扩展）。
**所有偏移访问前强制边界校验**，畸形文件返回错误而非越界。

## 5. 统一交互规范

```
oskit <command> [options] [operands]
```

- 退出码约定：`0` 成功；`1` 运行环境/文件错误；`2` 用法错误；
  `functest` 用非零表示有用例失败；`vuln --strict` 下高危发现返回 `2`。
- 所有工具 `--help` 输出用法；`oskit help <command>` 等价。
- 中文输出统一 UTF-8；JSON 输出为一行一个对象或单对象，字段见各工具文档。

## 6. 构建与测试

```
make            # 构建 ./oskit（-O2 -Wall -Wextra，零告警目标）
make test       # 冒烟测试：构建测试目标程序并逐工具验证（tests/run_tests.sh）
make clean
```

交叉工具链不是构建 oskit 本身的依赖，只影响 `port gen` 生成工程能否实际编译、
`port run` 能否启动 QEMU——缺失时工具给出明确提示与 `oskit port check` 引导。

## 7. 开发环境说明（WSL2）

- 项目源码位于 Windows 侧工作区（`/mnt/c/...`）以便编辑，构建与运行在 WSL 内进行；
- 本仓库交付时若系统未安装 gcc，可用免 root 工具链构建（见 `tests/run_tests.sh` 头部注释）；
- 推荐正式安装：`sudo apt install -y build-essential gdb qemu-system-x86
  qemu-system-misc qemu-system-arm gcc-riscv64-unknown-elf gcc-riscv64-linux-gnu
  gcc-arm-none-eabi gcc-aarch64-linux-gnu`。

## 8. 命名（待定）

当前工作名 `OSKit` 只出现在：README 标题、`Makefile` 的 `NAME`、`oskit version`
输出字符串。正式定名后改三处即可，代码接口不含项目名。
