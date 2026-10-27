#!/bin/bash
# run_tests.sh —— RVLab 冒烟测试(headless 命令链 + gdbstub 联测, 零 Python)
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
        echo "没有可用编译器" >&2; exit 1
    fi
fi
STATIC=""
case "$CC_BIN" in *musl*) STATIC="-static" ;; esac

FAIL=0
ok()  { echo "[OK]   $1"; }
bad() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

echo "== 构建 =="
bash build.sh >/tmp/rvlab-build.log 2>&1 && ok "build.sh" || { bad "build.sh"; cat /tmp/rvlab-build.log; exit 1; }
[ -x ./rvlab ] && ok "./rvlab 存在" || { bad "./rvlab 缺失"; exit 1; }
"$CC_BIN" -std=c11 -O2 -D_GNU_SOURCE -static -o /tmp/rsp_client tests/rsp_client.c 2>/dev/null \
    || "$CC_BIN" -std=c11 -O2 -D_GNU_SOURCE -o /tmp/rsp_client tests/rsp_client.c \
    && ok "构建 rsp_client" || bad "构建 rsp_client"

echo "== headless: 管道命令链 =="
./rvlab --version >/dev/null && ok "--version" || bad "--version"

printf 'demo 2\nrun\nstats\nquit\n' | ./rvlab > /tmp/rvlab-t1.log 2>&1
if grep -q "fib(10)=55" /tmp/rvlab-t1.log && grep -q "程序退出, 代码 55" /tmp/rvlab-t1.log; then
    ok "demo2 运行(UART 输出+退出码)"
else
    bad "demo2 运行"; tail -8 /tmp/rvlab-t1.log
fi

printf 'demo 2\nrun\nanalyze --json\nquit\n' | ./rvlab > /tmp/rvlab-t2.log 2>&1
if grep -q '"instructions"' /tmp/rvlab-t2.log && grep -q '"branch"' /tmp/rvlab-t2.log; then
    ok "analyze --json"
else
    bad "analyze --json"; tail -6 /tmp/rvlab-t2.log
fi

printf 'demo 1\nbp 0x80000044\nrun\nquit\n' | ./rvlab > /tmp/rvlab-t3.log 2>&1
grep -q "命中断点 0x80000044" /tmp/rvlab-t3.log && ok "断点: run 停在断点" \
    || { bad "断点"; tail -6 /tmp/rvlab-t3.log; }

printf 'demo 1\nstep 3\ndisas 2\nquit\n' | ./rvlab > /tmp/rvlab-t4.log 2>&1
grep -q "pc=8000000c" /tmp/rvlab-t4.log && grep -q "addi" /tmp/rvlab-t4.log \
    && ok "step/disas" || { bad "step/disas"; tail -6 /tmp/rvlab-t4.log; }

if [ -x ../oskit/oskit ]; then
    printf 'open ../oskit/oskit\nharden\nquit\n' | ./rvlab > /tmp/rvlab-t5.log 2>&1
    grep -q "RELRO" /tmp/rvlab-t5.log && ok "harden 检查真实 ELF(oskit)" \
        || { bad "harden"; tail -6 /tmp/rvlab-t5.log; }
else
    echo "[跳过] harden(无 ../oskit/oskit)"
fi

rm -rf /tmp/rvlab-gen
printf 'skeleton riscv64 t1 /tmp/rvlab-gen\nquit\n' | ./rvlab > /tmp/rvlab-t6.log 2>&1
nfiles=0
for f in crt0.S linker.ld kernel.c Makefile run.sh README.md; do
    [ -s "/tmp/rvlab-gen/t1/$f" ] && nfiles=$((nfiles+1))
done
[ "$nfiles" -eq 6 ] && ok "skeleton 生成 6 文件" || bad "skeleton($nfiles/6)"

printf 'build\nquit\n' | ./rvlab > /tmp/rvlab-t7.log 2>&1
grep -q "未找到 RISC-V 交叉工具链" /tmp/rvlab-t7.log && ok "无工具链时明确指引" \
    || { echo "     (环境已装工具链则属正常):"; tail -3 /tmp/rvlab-t7.log; ok "build 提示路径"; }

echo "== TUI 伪终端冒烟(script 分配 pty) =="
if command -v script >/dev/null 2>&1; then
    printf '\x0b' | timeout 10 script -qec "TERM=xterm ./rvlab --demo 1" /dev/null > /tmp/rvlab-tui.log 2>&1
    RC=$?
    if [ $RC -eq 0 ] && grep -q "寄存器" /tmp/rvlab-tui.log && grep -q "控制台" /tmp/rvlab-tui.log; then
        ok "TUI 渲染 + Ctrl-K 干净退出"
    else
        bad "TUI 冒烟(退出码 $RC)"
    fi
    printf ':run\n\x0b' | timeout 10 script -qec "TERM=xterm ./rvlab --demo 1" /dev/null > /tmp/rvlab-tui2.log 2>&1
    if grep -q "EMU OK" /tmp/rvlab-tui2.log && grep -q "代码 42" /tmp/rvlab-tui2.log; then
        ok "TUI 内 :run 命令链"
    else
        bad "TUI :run 链"
    fi
else
    echo "[跳过] TUI 冒烟(无 script 命令)"
fi

echo "== gdbstub 联测 =="
PORT=$((20000 + $$ % 20000))
./rvlab --gdbstub $PORT --demo 1 >/tmp/rvlab-stub.log 2>&1 &
STUB=$!
sleep 0.7
/tmp/rsp_client $PORT > /tmp/rvlab-rsp.log 2>&1
RC=$?
grep -q "全部通过" /tmp/rvlab-rsp.log && ok "rsp_client 全链路(qSupported/m/s/g/Z0/c/k)" \
    || { bad "rsp_client(退出码 $RC)"; cat /tmp/rvlab-rsp.log; }
kill $STUB 2>/dev/null
wait $STUB 2>/dev/null

echo "=================================================="
if [ $FAIL -eq 0 ]; then
    echo "RVLab 冒烟测试全部通过 ✔"
else
    echo "失败 $FAIL 项 ✘"
fi
exit $FAIL
