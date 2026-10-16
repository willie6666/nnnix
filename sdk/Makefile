# SPDX-License-Identifier: GPL-3.0-or-later
# nnnix SDK: build loadable programs (NXE files) with clang and ld.lld.
#   make                  every examples/*.c, into build/NAME.nxe
#   make build/foo.nxe    from foo.c here or in examples/
#   make STACK=2048 ...   task stack for the programs (default: the loader's 1024)
# Install a program with tools/nncp.py put build/foo.nxe /bin/foo, then run `foo`.

SDK     := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
CLANG   ?= clang
PYTHON  ?= python3
STACK   ?= 0
CFLAGS  = --target=arm-none-eabi -mcpu=cortex-m0 -mthumb -Os -g -ffreestanding -fno-builtin \
          -ffunction-sections -fdata-sections -fno-exceptions -Wall -Wextra -std=c11 \
          -I$(SDK)/include -I$(SDK)/../lib
LDFLAGS = -nostdlib -fuse-ld=lld -Wl,-T,$(SDK)/nxe.ld,--emit-relocs,--gc-sections

vpath %.c . $(SDK)/examples
PROGRAMS = $(patsubst $(SDK)/examples/%.c,build/%.nxe,$(wildcard $(SDK)/examples/*.c))

.PHONY: all clean
.SECONDARY:
all: $(PROGRAMS)

build/crt0.o: $(SDK)/crt0.c $(SDK)/include/nnnix.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/%.o: %.c $(SDK)/include/nnnix.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/%.elf: build/%.o build/crt0.o $(SDK)/nxe.ld
	$(CLANG) $(CFLAGS) $(LDFLAGS) build/crt0.o $< -o $@

build/%.nxe: build/%.elf $(SDK)/../tools/mknxe.py
	$(PYTHON) $(SDK)/../tools/mknxe.py --stack $(STACK) $< $@

clean:
	rm -rf build
