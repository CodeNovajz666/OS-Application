#!/bin/bash
# build.sh —— RVLab 构建(无 make 环境兜底; 复用 ../oskit 的 elf.c)
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

OSKIT_SRC="${OSKIT_SRC:-$ROOT/../oskit/src}"
if [ ! -f "$OSKIT_SRC/elf.c" ]; then
    echo "错误: 未找到 oskit 共享源码($OSKIT_SRC/elf.c)" >&2
    echo "      rvlab 复用 oskit 的 ELF 解析器, 请将 oskit 仓库置于同级目录," >&2
    echo "      或设 OSKIT_SRC=<oskit/src 路径>" >&2
    exit 1
fi

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

CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -Isrc -I$OSKIT_SRC"
LDFLAGS=""
case "$CC_BIN" in *musl*) LDFLAGS="-static" ;; esac

echo "== 构建 rvlab ($CC_BIN $LDFLAGS, 共享: $OSKIT_SRC) =="
"$CC_BIN" $CFLAGS -o rvlab src/*.c "$OSKIT_SRC/elf.c" $LDFLAGS
echo "== 完成: ./rvlab =="
