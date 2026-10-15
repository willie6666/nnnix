# nnnix build. Targets:
#   make            build build/nnnix.elf and build/nnnix.bin
#   make flash      program APROM through Nu-Link (tools/flash.py)
#   make test       flash + on-target tests (tools/nntest.py, needs sudo for the test netns)
#   make host-test  unit tests that run on the build machine
#   make size       section sizes

CLANG   ?= clang
HOSTCC  ?= cc
OBJCOPY ?= llvm-objcopy
SIZE    ?= llvm-size
PYTHON  ?= python3

VERSION := $(shell git describe --always --dirty --tags 2>/dev/null || echo unknown)

CFLAGS  = --target=arm-none-eabi -mcpu=cortex-m0 -mthumb -Os -g \
          -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
          -Wall -Wextra -Werror -std=c11 -DNNNIX_VERSION='"$(VERSION)"'
LDFLAGS = -nostdlib -fuse-ld=lld -Wl,-T,arch/nuc140/link.ld,--gc-sections,-Map,build/nnnix.map

SRCS = $(wildcard arch/nuc140/*.c kernel/*.c drivers/*.c lib/*.c)
OBJS = $(SRCS:%.c=build/%.o)

.PHONY: all flash test host-test size clean
all: build/nnnix.bin

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) -MMD -MP -c $< -o $@

build/nnnix.elf: $(OBJS) arch/nuc140/link.ld
	$(CLANG) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@
	@$(SIZE) $@

build/nnnix.bin: build/nnnix.elf
	$(OBJCOPY) -O binary $< $@

# Rebuild main.o when the git version changes.
build/kernel/main.o: build/.version
build/.version: FORCE
	@mkdir -p build
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@
FORCE:

flash: build/nnnix.bin
	$(PYTHON) tools/flash.py build/nnnix.bin

test: build/nnnix.bin
	sudo $(PYTHON) tools/nntest.py --flash build/nnnix.bin

HOST_TESTS = build/host/test_lib build/host/test_input
build/host/test_lib: tests/host/test_lib.c lib/fmt.c lib/string.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fno-builtin $^ -o $@
build/host/test_input: tests/host/test_input.c drivers/ch9350.c drivers/hidkey.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST $^ -o $@
host-test: $(HOST_TESTS)
	@for t in $(HOST_TESTS); do echo "== $$t"; (cd tests/host && ../../$$t) || exit 1; done

size: build/nnnix.elf
	$(SIZE) -A $<

clean:
	rm -rf build

-include $(OBJS:.o=.d)
