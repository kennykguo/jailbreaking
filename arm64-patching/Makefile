CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS ?= -I../include
PATCHER_PAGE_SIZE := $(shell getconf PAGESIZE 2>/dev/null || echo 4096)
CPPFLAGS += -DPATCHER_PAGE_SIZE=$(PATCHER_PAGE_SIZE)

TARGETS := patch_demo

all: $(TARGETS)

patch_demo: patch_demo.o patcher.o
	$(CC) $(CFLAGS) patch_demo.o patcher.o -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean
