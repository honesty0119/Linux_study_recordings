CC = gcc
AR = ar
BUILD_DIR ?= build/native
CPPFLAGS += -I.
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

SOURCES = add.c sub.c mul.c div.c
STATIC_OBJECTS = $(SOURCES:%.c=$(BUILD_DIR)/static/%.o)
PIC_OBJECTS = $(SOURCES:%.c=$(BUILD_DIR)/pic/%.o)
LIB_DIR = $(BUILD_DIR)/lib
BIN_DIR = $(BUILD_DIR)/bin

.PHONY: all static dynamic run
all: static dynamic
static: $(BIN_DIR)/main_static
dynamic: $(BIN_DIR)/main_dynamic

$(BUILD_DIR)/static/%.o: %.c calc.h
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/pic/%.o: %.c calc.h
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c "$<" -o "$@"

$(LIB_DIR)/libcalc.a: $(STATIC_OBJECTS)
	mkdir -p "$(@D)"
	$(AR) rcs "$@" $(STATIC_OBJECTS)

$(LIB_DIR)/libcalc.so: $(PIC_OBJECTS)
	mkdir -p "$(@D)"
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libcalc.so -o "$@" $(PIC_OBJECTS)

$(BIN_DIR)/main_static: main.c calc.h $(LIB_DIR)/libcalc.a
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c "$(LIB_DIR)/libcalc.a" -o "$@"

$(BIN_DIR)/main_dynamic: main.c calc.h $(LIB_DIR)/libcalc.so
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) main.c -L"$(LIB_DIR)" -lcalc -o "$@"

# Run only binaries built for this Linux machine.
run: all
	"./$(BIN_DIR)/main_static"
	LD_LIBRARY_PATH="$(abspath $(LIB_DIR))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" "./$(BIN_DIR)/main_dynamic"
