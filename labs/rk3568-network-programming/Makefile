# Ubuntu 构建：默认本机 GCC；交叉编译时显式传入 CC 与 BUILD_DIR。
CC = gcc
BUILD_DIR ?= build/native
CFLAGS ?= -O2 -g
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic
TARGETS = $(BUILD_DIR)/key_queue $(BUILD_DIR)/key_network $(BUILD_DIR)/key_udp $(BUILD_DIR)/test_core

.PHONY: all check
all: $(TARGETS)

$(BUILD_DIR)/key_queue: board/key_queue.c Makefile
	mkdir -p "$(BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -pthread board/key_queue.c $(LDFLAGS) -o "$@"

$(BUILD_DIR)/key_network: board/key_network.c board/task2_core.h Makefile
	mkdir -p "$(BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -pthread board/key_network.c $(LDFLAGS) -o "$@"

$(BUILD_DIR)/test_core: tests/test_core.c board/task2_core.h Makefile
	mkdir -p "$(BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) tests/test_core.c $(LDFLAGS) -o "$@"

$(BUILD_DIR)/key_udp: board/key_network.c board/udp_transport.inc board/task2_core.h Makefile
	mkdir -p "$(BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -DTASK2_UDP=1 -pthread board/key_network.c $(LDFLAGS) -o "$@"

check: all
	"$(BUILD_DIR)/test_core"
	"$(BUILD_DIR)/key_queue" --self-test
	python3 tests/test_udp_sender.py "$(BUILD_DIR)/key_udp"
