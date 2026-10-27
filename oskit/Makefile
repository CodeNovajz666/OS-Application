# OSKit —— 开源操作系统工具箱 (工作名, 名称待定)
# 改名: 修改 NAME 变量即可; 代码接口不含项目名
NAME ?= oskit
CC   ?= gcc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
LDLIBS = -pthread

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
HDR := $(wildcard src/*.h)

all: $(NAME)

$(NAME): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

# 冒烟测试: 构建测试目标并逐工具验证
test: all
	bash tests/run_tests.sh

clean:
	rm -f src/*.o $(NAME)

.PHONY: all test clean
