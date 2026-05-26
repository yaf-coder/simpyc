# simpyc — Makefile
#
# Targets:
#   make            -> build static lib + examples + tests
#   make lib        -> just the library
#   make test       -> run the test binaries
#   make bench      -> run benchmarks
#   make clean

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O3 -g -Wall -Wextra -Wpedantic -std=c11 -fno-omit-frame-pointer
ASFLAGS ?= -O3 -g
INCS    := -Iinclude -Isrc

UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
  CORO_ASM := src/coro_arm64.S
else ifeq ($(UNAME_M),aarch64)
  CORO_ASM := src/coro_arm64.S
else ifeq ($(UNAME_M),x86_64)
  CORO_ASM := src/coro_amd64.S
else
  $(error simpyc: unsupported architecture $(UNAME_M))
endif

LIB_SRCS := \
  src/heap.c \
  src/event.c \
  src/env.c \
  src/process.c \
  src/cond.c \
  src/resource.c \
  src/priority_resource.c \
  src/preemptive_resource.c \
  src/container.c \
  src/store.c \
  src/filter_store.c \
  src/priority_store.c \
  src/coro.c

LIB_OBJS := $(LIB_SRCS:.c=.o) $(CORO_ASM:.S=.o)

LIB      := libsimpyc.a

EXAMPLE_SRCS := $(wildcard examples/*.c)
EXAMPLES     := $(EXAMPLE_SRCS:.c=)

TEST_SRCS    := $(wildcard tests/*.c)
TESTS        := $(TEST_SRCS:.c=)

.PHONY: all lib test bench clean
all: lib $(EXAMPLES) $(TESTS)
lib: $(LIB)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) $(INCS) -c -o $@ $<

examples/%: examples/%.c $(LIB)
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LIB)

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LIB)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

bench: examples/bench
	./examples/bench

clean:
	rm -f $(LIB_OBJS) $(LIB) $(EXAMPLES) $(TESTS)
