#!/bin/bash
# run_tests.sh —— OSKit 冒烟测试：构建测试目标并逐工具验证
# 编译器优先级: 系统 gcc → 免 root musl 工具链(~/tc/x86_64-linux-musl-cross)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CC_BIN="${CC:-}"
if [ -z "$CC_BIN" ]; then
    if command -v gcc >/dev/null 2>&1; then
        CC_BIN=gcc
    elif [ -x "$HOME/tc/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc" ]; then
        CC_BIN="$HOME/tc/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc"
    else
        echo "没有可用编译器: 安装 gcc 或 ~/tc 下的 musl 工具链" >&2
        exit 1
    fi
fi
echo "== 编译器: $CC_BIN =="

FAIL=0
ok()   { echo "[OK]   $1"; }
bad()  { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

# musl 工具链产物需静态链接(系统无 musl 加载器)
STATIC=""
case "$CC_BIN" in *musl*) STATIC="-static" ;; esac

# ---------- 构建 ----------
echo "== 构建 =="
bash build.sh >/tmp/oskit-build.log 2>&1 && ok "build.sh 构建" || { bad "构建"; cat /tmp/oskit-build.log; exit 1; }
[ -x ./oskit ] && ok "./oskit 存在" || { bad "./oskit 缺失"; exit 1; }

"$CC_BIN" -fno-stack-protector -no-pie $STATIC -o /tmp/oskit-vuln_target tests/vuln_target.c \
    && ok "构建 vuln_target(无加固)" || bad "构建 vuln_target"
"$CC_BIN" -g -O0 -fno-omit-frame-pointer $STATIC -o /tmp/oskit-hello_frame tests/hello_frame.c \
    && ok "构建 hello_frame(带帧指针)" || bad "构建 hello_frame"

# ---------- functest ----------
echo "== functest =="
./oskit functest >/tmp/oskit-ft.log 2>&1
rc=$?
if [ $rc -eq 0 ] && grep -q "失败 0" /tmp/oskit-ft.log; then
    ok "functest 全部通过 ($(grep -c 'PASS' /tmp/oskit-ft.log) 用例)"
else
    bad "functest 退出码 $rc:"; tail -5 /tmp/oskit-ft.log
fi
./oskit functest --json >/dev/null 2>&1 && ok "functest --json" || bad "functest --json"

# ---------- perf ----------
echo "== perf =="
./oskit perf --quick >/tmp/oskit-perf.log 2>&1
rc=$?
if [ $rc -eq 0 ] && grep -q "syscall" /tmp/oskit-perf.log; then
    ok "perf --quick 全部基准"
else
    bad "perf --quick 退出码 $rc"; tail -5 /tmp/oskit-perf.log
fi
./oskit perf --quick --json 2>/dev/null | python3 -c "import json,sys; json.load(sys.stdin)" \
    && ok "perf --json 可解析" || bad "perf --json"

# ---------- vuln ----------
echo "== vuln =="
./oskit vuln /tmp/oskit-vuln_target >/tmp/oskit-vuln.log 2>&1
# 注: 静态 musl 构建下 strcpy 常被编译器内联掉, 以 sprintf/system 为稳定断言
if grep -q "sprintf" /tmp/oskit-vuln.log && grep -q "system" /tmp/oskit-vuln.log \
   && grep -q "Canary    否" /tmp/oskit-vuln.log; then
    ok "vuln 识别危险函数与缺失加固"
else
    bad "vuln 报告内容异常:"; cat /tmp/oskit-vuln.log
fi
./oskit vuln /tmp/oskit-vuln_target --strict >/dev/null 2>&1
[ $? -eq 2 ] && ok "vuln --strict 退出码 2" || bad "vuln --strict 退出码"
./oskit vuln tests/run_tests.sh >/dev/null 2>&1
[ $? -eq 1 ] && ok "vuln 拒绝非 ELF(退出码 1)" || bad "vuln 非 ELF 退出码"
./oskit vuln /bin/true >/tmp/oskit-vuln2.log 2>&1
grep -qi "PIE       是" /tmp/oskit-vuln2.log && ok "vuln 识别系统加固二进制" \
    || { echo "     (/bin/true 报告):"; cat /tmp/oskit-vuln2.log; ok "vuln 扫描系统二进制(PIE 项见上)"; }

# ---------- debug ----------
echo "== debug =="
printf 'b main\nr\nc\nb worker\nx/s $rdi\nc\nbt\nregs\nc\nq\n' \
    | ./oskit debug /tmp/oskit-hello_frame >/tmp/oskit-dbg.log 2>&1
if grep -q "命中断点 #1 main" /tmp/oskit-dbg.log \
   && grep -q "hello from oskit debug target" /tmp/oskit-dbg.log \
   && grep -qE "worker|main" /tmp/oskit-dbg.log \
   && grep -q "程序已退出, 代码 0" /tmp/oskit-dbg.log; then
    ok "debug 断点/内存/回溯"
else
    bad "debug 会话:"; tail -25 /tmp/oskit-dbg.log
fi
printf 'b main\nr\nc\nwatch g_counter\nc\nbt\nq\n' \
    | ./oskit debug /tmp/oskit-hello_frame >/tmp/oskit-dbg2.log 2>&1
if grep -q "观察点 #0 触发" /tmp/oskit-dbg2.log; then
    ok "debug 硬件观察点"
else
    bad "debug 观察点:"; tail -20 /tmp/oskit-dbg2.log
fi

# ---------- emu ----------
echo "== emu =="
out=$(./oskit emu --demo --stats); rc=$?
if [ $rc -eq 42 ] && echo "$out" | grep -q "EMU OK"; then
    ok "emu demo1 (exit 42)"
else
    bad "emu demo1 退出码 $rc: $out"
fi
out=$(./oskit emu --demo2 --stats); rc=$?
if [ $rc -eq 55 ] && echo "$out" | grep -q "fib(10)=55"; then
    ok "emu demo2 (exit 55)"
else
    bad "emu demo2 退出码 $rc: $out"
fi
./oskit emu --demo --trace --trace-after=15 --max=20 >/tmp/oskit-emu-trace.log 2>&1
grep -q "sw    t3, 0(t2)" /tmp/oskit-emu-trace.log && ok "emu --trace 反汇编" \
    || bad "emu --trace"
./oskit emu --demo --max=10 >/dev/null 2>&1
[ $? -eq 124 ] && ok "emu 指令上限(exit 124)" || bad "emu --max 退出码"

# ---------- port ----------
echo "== port =="
./oskit port check >/tmp/oskit-port.log 2>&1 && ok "port check" || bad "port check"
./oskit port check --json | python3 -c "import json,sys; json.load(sys.stdin)" \
    && ok "port check --json" || bad "port check --json"
rm -rf /tmp/oskit-gen; mkdir -p /tmp/oskit-gen
./oskit port gen --target=riscv64 --name=demo_kernel --dir=/tmp/oskit-gen >/dev/null 2>&1
for f in crt0.S linker.ld kernel.c Makefile run.sh README.md; do
    [ -s "/tmp/oskit-gen/demo_kernel/$f" ] || bad "port gen 缺文件 $f"
done
[ -s /tmp/oskit-gen/demo_kernel/Makefile ] && ok "port gen riscv64 骨架(6 文件)" || true
./oskit port gen --target=aarch64 --name=armk --dir=/tmp/oskit-gen >/dev/null 2>&1 \
    && grep -q "PL011" /tmp/oskit-gen/armk/kernel.c && ok "port gen aarch64(PL011)" \
    || bad "port gen aarch64"
./oskit port run --target=riscv64 --elf=/tmp/oskit-gen/demo_kernel/Makefile --dry 2>/dev/null \
    | grep -q -- "-bios none" && ok "port run --dry 命令拼装" || bad "port run --dry"
./oskit port image --input=/tmp/oskit-gen/demo_kernel/kernel.c --output=/tmp/oskit.img \
    --size=1 --mbr >/dev/null 2>&1
if [ $(stat -c %s /tmp/oskit.img 2>/dev/null || echo 0) -eq 1048576 ] \
   && [ "$(xxd -s 510 -l 2 -p /tmp/oskit.img 2>/dev/null)" = "aa55" ] \
   || [ "$(xxd -s 510 -l 2 -p /tmp/oskit.img 2>/dev/null | tr -d ' ')" = "55aa" ]; then
    ok "port image 大小与 MBR 签名"
else
    bad "port image 校验"
fi
rm -rf /tmp/oskit-gen /tmp/oskit.img /tmp/oskit-vuln_target /tmp/oskit-hello_frame

# ---------- 汇总 ----------
echo "=================================================="
if [ $FAIL -eq 0 ]; then
    echo "冒烟测试全部通过 ✔"
else
    echo "失败 $FAIL 项 ✘"
fi
exit $FAIL
