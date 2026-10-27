# 03 · 漏洞分析工具（`oskit vuln`）

## 1. 目标与定位

对 ELF 可执行文件/共享对象做**静态安全体检**，从两个维度发现可被利用的薄弱面：

1. **加固缺失**（hardening）：NX、PIE、RELRO、栈保护、FORTIFY —— 编译/链接期
   安全机制是否开启；
2. **危险接口使用**（dangerous APIs）：是否导入/使用无边界检查的 C 库函数
   （`gets`/`strcpy`/`sprintf`...）或易被注入的接口（`system`/`popen`）。

定位是**快速、零依赖的第一道筛查**（对标 `checksec` + 轻量符号扫描的合体），
不是漏洞证明工具：报告的是"风险面"，不是"可利用漏洞"；确认需人工审计或动态测试。

## 2. 检查项明细

| 检查 | 判定依据（ELF 结构）| 级别 |
|------|--------------------|------|
| NX（栈不可执行）| 缺少 `PT_GNU_STACK`，或该段 `p_flags` 含 `PF_X` | 高危 |
| PIE（地址随机化）| `e_type == ET_EXEC`（非位置无关）| 中危 |
| Full RELRO | 有 `PT_GNU_RELRO` 且动态段含 `DT_BIND_NOW`/`DF_1_NOW` | — |
| Partial RELRO | 有 `PT_GNU_RELRO` 但无 BIND_NOW（GOT 仍可写）| 低危 |
| No RELRO | 无 `PT_GNU_RELRO` 段 | 高危 |
| Stack Canary | 符号表中无 `__stack_chk_fail`（未启用 `-fstack-protector`）| 高危 |
| FORTIFY | 无任何 `__*_chk` 符号（未启用 `-D_FORTIFY_SOURCE`）| 低危 |
| 危险函数 | `.dynsym`/`.symtab` 中出现下表函数 | 按函数级别 |
| RPATH/RUNPATH | 动态段 `DT_RPATH`/`DT_RUNPATH` 存在（可被共享库注入）| 低危 |
| 符号信息 | 无 `.symtab`（stripped）→ 静态分析受限提示 | 提示 |

危险函数分级：

| 级别 | 函数 | 原因 |
|------|------|------|
| 高 | `gets` | 无法限制长度，C11 已删除 |
| 高 | `strcpy` `strcat` `sprintf` `vsprintf` | 目标缓冲区无界 |
| 高 | `scanf` `fscanf` `sscanf`（不带宽度）| `%s` 无界写入 |
| 中 | `system` `popen` | 命令注入面 |
| 中 | `strtok` `getenv`（结果直接用作路径等）| 可重入/环境不可信 |
| 低 | `strncpy` `strncat` | 有界但仍可能不终止 |

## 3. 架构

```
        vuln.c
 ┌────────────┐   ┌───────────────────────┐
 │ 读入文件    │──▶│ elf.c 共享解析器        │
 └────────────┘   │  程序头: GNU_STACK/    │
                  │         GNU_RELRO      │
                  │  动态段: BIND_NOW/     │
                  │         RPATH/NEEDED   │
                  │  符号:  symtab+dynsym  │
                  └──────────┬────────────┘
                             ▼
 ┌─────────────────────────────────────────┐
 │ 规则表 g_rules[]: {名称, 级别, 判定fn}    │
 └────────────────┬────────────────────────┘
                  ▼
      文本报告（清单+发现+建议） / --json
```

- 动态段解析：`PT_DYNAMIC` → `{d_tag,d_val}` 数组 → 字符串类条目
  （`DT_NEEDED`/`DT_RPATH`）经 `elf_vaddr_to_off()`（vaddr→PT_LOAD 文件偏移）取串；
- 符号扫描复用 `elf_for_each_sym()`，先 `.symtab`（全量）后 `.dynsym`（导入），
  对静态链接二进制 `.dynsym` 为空仍能从 `.symtab` 发现。

## 4. 原理备注（教学要点）

- **NX**：`PT_GNU_STACK` 是 Linux 专有的程序头，`p_flags` 无 `X` 时栈映射为 RW；
  缺失则内核回退为可执行栈（老旧 ABI 兼容）。
- **RELRO**：重定位只读。Partial 把 `.init_array/.dynamic` 等重排进只读页；
  Full 再加 `BIND_NOW`（加载即完成全部符号绑定后把 GOT 置只读），
  使 GOT 覆写类漏洞失效。判定 BIND_NOW 的两个来源：`DT_FLAGS` 的
  `DF_BIND_NOW`、`DT_FLAGS_1` 的 `DF_1_NOW`。
- **Canary**：`-fstack-protector` 生成的函数在序言埋随机值、尾声校验，
  校验失败调用 `__stack_chk_fail` —— 因此该符号存在即证明至少部分函数受保护。
- **FORTIFY**：`_FORTIFY_SOURCE` 将 `memcpy/strcpy/...` 在编译期可判长度时
  替换为带边界的 `__memcpy_chk` 等符号。

## 5. CLI 接口

```
oskit vuln <文件> [--json] [--strict] [--verbose]
```

| 参数 | 说明 |
|------|------|
| `--json` | 机器格式：`{file, arch, pie, nx, relro, canary, fortify, findings:[{level,symbol,desc}]}` |
| `--strict` | 存在高危发现时退出码 `2`（供 CI 门禁）|
| `--verbose` | 附带打印全部 NEEDED 依赖与 RPATH 值 |

退出码：`0` 分析完成（无论是否有发现）；`1` 文件不存在/非 ELF/解析失败；`2` strict 模式命中高危。

## 6. 输出示例

```
== OSKit 漏洞分析: tests/vuln_target ==
架构      x86-64, 动态链接, 含符号表
PIE       否        [中] 建议 -fPIE -pie
NX        是
RELRO     Partial   [低] 建议 -Wl,-z,relro,-z,now
Canary    否        [高] 建议 -fstack-protector-strong
FORTIFY   否        [低] 建议 -O2 -D_FORTIFY_SOURCE=2
危险函数  3 个
  [高] gets       无长度限制, 已从标准移除
  [高] strcpy     目标缓冲区无界
  [中] system     命令注入风险
结论      高危 2 · 中危 1 · 低危 2
```

## 7. 验收标准

1. 对 `gcc -fno-stack-protector -no-pie` 编译的 `tests/vuln_target` 报告
   Canary 缺失 + PIE 缺失 + gets/strcpy/system；
2. 对 `/bin/ls`（Ubuntu 默认加固）报告 PIE/RELRO/Canary 开启；
3. 传入非 ELF 文件（如脚本文本）报错退出码 1，且不崩溃；
4. 畸形 ELF（截断、节头越界）被边界检查拦截，给出解析失败而非越界访问。

## 8. 已知限制与扩展方向

- 只读符号名，不反汇编、不做调用图/污点分析（危险符号"存在"不等于"可利用路径存在"）；
- 静态链接 PIE、内核模块等特殊形态只做基本判定；
- 扩展方向：借 DWARF 区分危险函数是否在可达路径；对接 `--compare` 输出加固基线；
  对 `.note.gnu.property`（CET/IBT）的识别。
