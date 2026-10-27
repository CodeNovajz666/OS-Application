# 06 · 系统移植工具（`oskit port`）

## 1. 目标与定位

把"把系统/内核移植到新目标机"里**可模板化的部分**自动化：

1. **环境体检（check）**：盘点交叉工具链与 QEMU 是否就绪；
2. **工程骨架生成（gen）**：为 5 种目标机一键生成可编译的裸机工程
   （启动代码 + 链接脚本 + 串口驱动 + Makefile + 运行说明）；
3. **QEMU 启动（run）**：按目标机拼装正确的 QEMU 命令行并启动/打印（--dry）；
4. **磁盘镜像（image）**：生成裸磁盘镜像骨架（MBR 签名 + 内核摆放），
   为引导加载器实验铺路。

一句话：`oskit port gen` 之后 `make && oskit port run` 就能在 QEMU 里看到
自己内核的第一行输出——移植的"从 0 到串口打印 hello"被压缩到一分钟。

## 2. 目标机矩阵

| target | 机器(QEMU) | 内存基址 | 串口 | 交叉前缀（默认，可覆盖）|
|--------|-----------|---------|------|------------------------|
| `riscv32` | `-M virt -bios none` | 0x8000_0000 | NS16550 @0x1000_0000 | `riscv32-unknown-elf-` |
| `riscv64` | `-M virt -bios none` | 0x8000_0000 | NS16550 @0x1000_0000 | `riscv64-unknown-elf-`（或 `-linux-gnu-`）|
| `armv7`   | `-M virt` | 0x4000_0000 | PL011 @0x0900_0000 | `arm-none-eabi-` |
| `aarch64` | `-M virt` | 0x4000_0000 | PL011 @0x0900_0000 | `aarch64-linux-gnu-` |
| `i386`    | `-kernel`(Multiboot) | 0x0010_0000 | COM1 0x3F8（轮询）| （宿主 gcc `-m32`，需 gcc-multilib）|

> QEMU `virt` 机型内存映射是开源社区事实上的教学标准（与 `oskit emu` 的设备地址约定一致），
> 实机移植时只需按芯片手册改链接脚本基址与 UART 地址。

## 3. 架构

```
port.c
├── check: 工具清单探测表 → 逐项 command -v + --version → 表格/JSON
├── gen:   target_desc 表(地址/前缀/QEMU参数/模板) → 写出 6 个文件:
│          crt0.S · linker.ld · kernel.c · Makefile · run.sh · README.md
├── run:   target_desc 拼装 argv → execvp(qemu...) 或 --dry 打印
└── image: ftruncate 建空白镜像 → 可选 --mbr 写签名与占位引导扇区
           → 把内核文件写至 --offset（默认 512）
```

每个目标机是一行 `target_desc`（基址、RAM 大小、串口类型、QEMU 参数、交叉前缀），
新增目标机 = 加一行描述 + （必要时）一套模板，符合开闭原则。

## 4. 生成物说明（以 riscv64 为例）

```
myos/
├── crt0.S      _start: 设 gp(全局指针,配合链接器松弛) → 设 sp(__stack_top=RAM顶)
│               → call kmain → wfi 自旋
├── linker.ld   ENTRY(_start)；MEMORY{ram: ORIGIN=0x80000000, LENGTH=128M }；
│               .text/.rodata/.data/.bss 就位；导出 __bss_start/__bss_end、
│               __stack_top、__global_pointer$
├── kernel.c    uart_init/uart_putc/uart_puts（NS16550：查 LSR.bit5 再写 THR）
│               + kmain(): 打印横幅后 wfi 循环
├── Makefile    CROSS?=riscv64-unknown-elf-；-ffreestanding -nostdlib -O2
│               -fno-builtin -fno-stack-protector -T linker.ld；make run=打包 qemu
├── run.sh      等价于 oskit port run --target riscv64 --elf myos.elf
└── README.md   该目标机的编译/运行/改内存映射说明
```

各目标机差异点：

- **armv7/aarch64**：PL011 先查 FR.TXFF 再写 DR；crt0 用 `ldr sp, =__stack_top`；
- **i386**：Multiboot 头（magic `0x1BADB002`）须位于文件前 8KiB —— 放在
  `.multiboot` 段并置于链接脚本最前；串口用 `outb/inb` 内联汇编轮询 COM1；
  栈用 .bss 中静态数组（无加载器设栈）；
- **riscv**：`-bios none` 使 QEMU 直接跳到 0x8000_0000（否则先跑 OpenSBI）。

## 5. CLI 接口

```
oskit port check [--json]
oskit port gen  --target <t> --name <工程名> [--dir <输出目录>] [--force]
oskit port run  --target <t> --elf <文件> [--dry] [-- qemu参数...]
oskit port image --input <文件> --output <img> [--size MiB] [--offset N] [--mbr]
```

- `gen` 默认在当前目录创建 `<name>/`；已存在则拒绝（`--force` 覆盖）；
- `run --dry` 只打印将执行的 QEMU 命令（教学与排错友好）；
- `image --mbr` 写入 512 字节引导扇区骨架：0x7C0 处打印提示消息的死循环代码
  + 0x1FE 处 `55 AA` 签名（明确声明是占位，不是可用引导器）。

退出码：`0` 成功；`1` 目标缺失/QEMU 不存在/文件错误；`2` 用法错误（含未知 target）。

## 6. 验收标准

1. `oskit port check` 正确报告 WSL 内已装/缺失工具（与 `command -v` 结果一致）；
2. 五种 target 均能生成工程；目录/文件齐全；重复生成不加 `--force` 报错；
3. 装有对应工具链与 QEMU 的环境下：`gen → make → run` 能在串口打印横幅
   （WSL 演示环境未装 QEMU 时，用 `run --dry` 验证命令行拼装正确性）；
4. `image` 产物大小、`55 AA` 签名、内核偏移可用 `xxd` 检验。

## 7. 已知限制与扩展方向

- i386 目标依赖 `gcc-multilib`（`-m32`），Ubuntu 默认未装时 make 报错并提示；
- 骨架不含中断/定时器/MMU 初始化——刻意保持"最小可运行"，是实验的起点而非终点；
- 扩展路线：生成 SD 卡 FAT32 镜像并放入内核（替代裸 image）、U-Boot `mkimage` 打包、
  DTB 生成、以及与 `oskit emu` 的地址约定完全打通（emu 直接跑 port 产物）。
