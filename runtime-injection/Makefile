CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS ?= -I../include

UNAME_S := $(shell uname -s)

TARGETS := injector target

ifeq ($(UNAME_S),Darwin)
DYLIB_EXT := dylib
DYLIB_LDFLAGS := -dynamiclib
OBJCLIBS := -lobjc
else
DYLIB_EXT := so
DYLIB_LDFLAGS := -shared
OBJCLIBS :=
endif

all: $(TARGETS) hook.$(DYLIB_EXT)

injector: injector.o
	$(CC) $(CFLAGS) injector.o -o $@

target: target.o
	$(CC) $(CFLAGS) target.o -o $@ $(OBJCLIBS)

hook.$(DYLIB_EXT): hook.o
	$(CC) $(CFLAGS) $(DYLIB_LDFLAGS) hook.o -o $@ $(OBJCLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGETS) hook.$(DYLIB_EXT) *.o

.PHONY: all clean
