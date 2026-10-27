# 01 · 性能测试工具（`oskit perf`）

## 1. 目标与定位

对**宿主操作系统**（Linux/WSL2）的处理器、内存子系统、系统调用层、进程/线程管理、
磁盘 I/O 路径进行微基准（microbenchmark）测量，输出可复现的统计分布（而非单次采样），
用于：操作系统课程实验、内核/库升级前后对比、虚拟化（WSL2）开销观察。

不做整机应用级测试（无 SPEC/Web 之类），也不做压力/稳定性测试——那是 functest 与外部工具的范畴。

## 2. 功能范围

| 基准项 | 测量对象 | 指标 |
|--------|----------|------|
| `cpu-int` | 寄存器整数运算流水 | Mops/s（混合 xor/add/imul 循环）|
| `cpu-mem` | 内存拷贝带宽 | MB/s，按 4K/64K/1M/16M 分档呈现 L1/L2/RAM 台阶 |
| `mem-latency` | 依赖加载访存延迟 | ns/次（随机环形指针追逐，预取失效）|
| `mem-alloc` | 堆分配器吞吐 | 万次/s（malloc/free 小对象）|
| `syscall` | 系统调用进出开销 | ns/次（`getpid` 热循环，不经过 vDSO）|
| `ctx-switch` | 进程间切换开销 | ns/次（管道乒乓往返折半）|
| `fork` | 进程创建+回收 | µs/次（fork + _exit + waitpid）|
| `thread` | 线程创建+回收 | µs/次（pthread_create + join）|
| `disk-seq-write` | 顺序写带宽 | MB/s（1MiB 块写临时文件，不 fsync）|
| `disk-seq-read` | 顺序读带宽 | MB/s（写后回读，页缓存命中需注意）|
| `disk-fsync` | 持久化延迟 | ms/次（4KiB 写 + fsync）|

## 3. 架构

```
        perf.c
 ┌────────────────────┐
 │ 基准注册表 g_bench[]│  每项: {名称, 中文说明, 单位, run(quick, &out)}
 └─────────┬──────────┘
   选择过滤│(全部/指定/quick)
 ┌─────────▼──────────┐    ┌────────────────────┐
 │ 运行器: 每项执行 N 次 │───▶│ common: samples 统计 │
 │ N = quick? 少: 多     │    │ min/med/avg/p95/max │
 └─────────┬──────────┘    └────────────────────┘
           ▼
   文本表格 / --json
```

- 每个基准是一个自包含函数，向样本集压入多次测量值；
- 采样次数：常规 5~11 次（按单项耗时自适应），`--quick` 约 3 次并缩减负载规模；
- 统一采用 `CLOCK_MONOTONIC`（`common.h: now_ns()`），避免墙上时钟跳变。

## 4. 关键方法论（文档即实验依据）

1. **统计而非单点**：调度抖动、CPU 频率爬升、邻居进程都会污染单次测量；
   报告 min（物理下界近似）、median（典型值）、p95（尾延迟）、max（异常上界）。
2. **预热**：每项先跑 1 次不计入（预热分支预测器/缓存/页缓存/频率）。
3. **防优化消除**：整型循环累积到 `volatile` 汇聚变量并在末尾打印条件分支；
   拷贝循环使用不同源/目的缓冲且校验和输出。
4. **指针追逐测延迟**：预先生成覆盖数组的随机单环（Fisher–Yates 洗成环），
   每次加载依赖上一次结果，硬件预取器无法命中。
5. **上下文切换的乒乓模型**：父子进程各持管道一端，1 字节交替写/读 R 次，
   `时间/R/2` 记为"每次切换"，与 lmbench 的 `lat_ctx` 同口径。
6. **磁盘测试的诚实性**：输出 `statfs` 探测到的文件系统类型（tmpfs 会呈现内存级带宽，
   结果须按类型解读）；临时文件创建于 `/tmp`，写完即 `unlink`，异常路径也清理。

## 5. CLI 接口

```
oskit perf [--list] [--quick] [--json] [基准名 ...]
```

| 参数 | 说明 |
|------|------|
| （无基准名）| 顺序运行全部 11 项 |
| `--list` | 列出全部基准名与说明，不运行 |
| `--quick` | 快速模式：样本少、负载小（约 1/8 规模），适合脚本/CI 冒烟 |
| `--json` | 输出 JSON 数组：`[{name,desc,unit,samples:{min,median,mean,p95,max},detail:[...]}]` |

退出码：`0` 正常；`1` 运行期错误（如磁盘临时文件创建失败）；`2` 用法错误。

## 6. 输出示例

```
== OSKit 性能测试 (quick) ==
cpu-int        整数运算        Mops/s   min 812.4  med 845.1  avg 843.7  p95 871.2  max 875.0
cpu-mem[4K]    拷贝带宽        MB/s     ...（每档一行）
...
磁盘临时目录 /tmp (ext4)
disk-seq-write 顺序写          MB/s     ...
```

## 7. 核心数据结构

```c
typedef struct bench {
    const char *name, *desc, *unit;
    int  (*run)(int quick, bench_out *out);   /* 返回 0 成功 */
} bench_t;
/* bench_out: samples 数组 + 可选明细行(cpu-mem 各容量档) */
```

## 8. 验收标准

1. `oskit perf --quick` 在 WSL2 Ubuntu 22.04 上全绿退出，总时长 < 60s；
2. `cpu-mem` 各档带宽呈单调不增（L1 > L2 > RAM 数量级关系）；
3. `syscall` 结果量级在 50~500ns（WSL2 轻度波动可接受）；
4. `--json` 输出可被 `python3 -c "import json,sys; json.load(sys.stdin)"` 解析；
5. 中途 Ctrl-C 不留下临时文件（信号安全：临时文件创建后立即 unlink）。

## 9. 已知限制与扩展方向

- 不设 CPU 亲和性/实时调度（需特权且 WSL 下收益不稳定），数据波动靠统计缓解；
- `disk-seq-read` 测的是"页缓存命中读"与"回源读"的混合，深冷读需 `posix_fadvise(DONTNEED)`（留扩展）；
- 扩展方向：futex 延迟、mmap 缺页开销、透明大页对比、`--compare A.json B.json` 对比报告。
