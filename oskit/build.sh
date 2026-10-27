#!/bin/bash
# build.sh —— 无 make 环境的兜底构建(与 Makefile 等价)
# 优先系统 gcc; 否则用免 root musl 工具链 ~/tc/x86_64-linux-musl-cross
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

CC_BIN="${CC:-}"
if [ -z "$CC_BIN" ]; then
    if command -v gcc >/dev/null 2>&1; then
        CC_BIN=gcc
    elif [ -x "$HOME/tc/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc" ]; then
        CC_BIN="$HOME/tc/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc"
    else
        echo "错误: 找不到编译器 (安装 gcc, 或准备 ~/tc musl 工具链, 或 CC=... $0)" >&2
        exit 1
    fi
fi

CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE"
# musl 工具链默认动态链接且系统无 musl 加载器 → 静态链接; 系统 gcc 静态亦可运行
LDFLAGS=""
if ! command -v gcc >/dev/null 2>&1 || [ "${CC:-}" != "" ]; then
    case "$CC_BIN" in *musl*) LDFLAGS="-static" ;; esac
fi
echo "== 构建 oskit ($CC_BIN $LDFLAGS) =="
"$CC_BIN" $CFLAGS -o oskit src/*.c -pthread $LDFLAGS
echo "== 完成: ./oskit =="
