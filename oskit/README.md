# OSKit —— 开源操作系统工具箱（工作名，名称待定）

一套运行于 **Linux（开发环境：WSL2 · Ubuntu 22.04）** 之上的开源操作系统研究/开发工具集，
围绕开源操作系统（Linux 内核、RISC-V/ARM 裸机系统等）的开发与教学场景，提供六大工具：

| 子命令 | 工具 | 一句话说明 |
|--------|------|-----------|
| `oskit perf` | 性能测试 | CPU / 内存 / 系统调用 / 进程线程 / 磁盘 I/O 微基准测试，输出统计分布 |
| `oskit functest` | 功能测试 | Linux 系统调用与 POSIX 行为功能测试（文件/内存/进程/信号/管道/目录/时间） |
| `oskit vuln` | 漏洞分析 | ELF 可执行文件静态安全检查（NX/PIE/RELRO/Canary/Fortify + 危险函数扫描） |
| `oskit debug` | 软件调试 | 基于 ptrace 的迷你调试器：断点、单步、寄存器、内存、硬件观察点、回溯 |
| `oskit emu` | 系统模拟器 | RISC-V RV32I+M 指令集模拟器（ELF 加载、MMIO 设备、指令跟踪） |
| `oskit port` | 系统移植 | 裸机工程骨架生成器（5 种目标机）+ 交叉环境体检 + QEMU 启动 + 磁盘镜像 |

## 设计原则

1. **零第三方依赖**：纯 C11 + Linux 系统调用（仅 libc + pthread），`gcc + make` 即可构建；
2. **单二进制多子命令**：一个 `oskit` 可执行文件承载全部工具，便于分发与教学；
3. **可脚本化**：所有工具支持非交互运行，关键工具提供 `--json` 输出，可接入 CI；
4. **文档先行**：`docs/` 下每个工具一份技术文档，接口与实现一一对应。

## 快速开始（WSL Ubuntu 内）

```bash
cd oskit
make                 # 生成 ./oskit（无 make/gcc 时: bash build.sh, 自动找免 root 工具链）
./oskit help         # 总帮助
./oskit perf --quick # 性能测试（快速模式）
./oskit functest     # 功能测试
./oskit vuln /bin/ls # 漏洞分析示例
./oskit emu --demo   # 运行内置 RISC-V 演示程序
./oskit port check   # 交叉编译环境体检
bash tests/run_tests.sh   # 冒烟测试（24 项）
```

推荐正式环境安装（需 sudo）：

```bash
sudo apt update
sudo apt install -y build-essential gdb \
    qemu-system-x86 qemu-system-misc qemu-system-arm \
    gcc-riscv64-unknown-elf gcc-riscv64-linux-gnu \
    gcc-arm-none-eabi gcc-aarch64-linux-gnu
```

## 目录结构

```
oskit/
├── README.md            本文件
├── Makefile             构建脚本
├── docs/                技术文档（00 总览 + 六大工具各一份）
├── src/
│   ├── main.c           CLI 入口与子命令分发
│   ├── common.c/.h      公共工具（计时/统计/JSON/文件读取）
│   ├── elf.c/.h         共享的 ELF32/ELF64 解析器
│   ├── perf.c           性能测试
│   ├── functest.c       功能测试
│   ├── vuln.c           漏洞分析
│   ├── debug.c          软件调试器（x86-64）
│   ├── emu.c            RISC-V 系统模拟器
│   └── port.c           系统移植
└── tests/               测试目标程序与冒烟测试脚本
```

## 文档索引

- [docs/00-overview.md](docs/00-overview.md) 总体架构与技术选型
- [docs/01-perf.md](docs/01-perf.md) 性能测试工具
- [docs/02-functest.md](docs/02-functest.md) 功能测试工具
- [docs/03-vuln.md](docs/03-vuln.md) 漏洞分析工具
- [docs/04-debug.md](docs/04-debug.md) 软件调试工具
- [docs/05-emu.md](docs/05-emu.md) 系统模拟器
- [docs/06-port.md](docs/06-port.md) 系统移植工具

## 许可与命名

代码按 MIT 许可发布（可按需修改）。项目名 **OSKit** 为工作名（名称待定），
改名只需修改 `Makefile` 中的 `NAME` 变量与 README 标题，接口不含项目名，无连带影响。

## 已知限制

- `debug` 子命令仅支持 x86-64 宿主（依赖 `PTRACE_GETREGS` 与 x86-64 寄存器布局）；
- `emu` 实现 RV32I + M 扩展（不含压缩指令/原子指令/S 模式），定位为教学级模拟器；
- `vuln` 为静态分析（不含反汇编与污点追踪），动态符号在静态链接二进制中不可见；
- `port` 生成的裸机骨架面向 QEMU `virt` 系列机型，实机移植需按手册调整内存映射。
