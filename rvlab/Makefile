# RVLab —— RISC-V 系统开发工作台 (工作名)
# 复用 ../oskit 的 ELF 解析器(单一事实来源, 避免两份拷贝漂移)
NAME     ?= rvlab
CC       ?= gcc
OSKIT_SRC ?= ../oskit/src
CFLAGS   ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
            -Isrc -I$(OSKIT_SRC)
LDLIBS   =

SRC  := $(wildcard src/*.c) $(OSKIT_SRC)/elf.c

all: $(NAME)

$(NAME): $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

test: all
	bash tests/run_tests.sh

clean:
	rm -f $(NAME)

.PHONY: all test clean
