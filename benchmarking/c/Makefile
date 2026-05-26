# Builds the C factory against libsimpyc.a from the repo root.

CC      ?= cc
CFLAGS  ?= -O3 -g -Wall -Wextra -Wpedantic -std=c11 -fno-omit-frame-pointer
ROOT    := ../..
INCS    := -I$(ROOT)/include

LIB     := $(ROOT)/libsimpyc.a

.PHONY: all clean
all: factory

factory: factory.c $(LIB)
	$(CC) $(CFLAGS) $(INCS) -o $@ factory.c $(LIB)

$(LIB):
	$(MAKE) -C $(ROOT) lib

clean:
	rm -f factory
