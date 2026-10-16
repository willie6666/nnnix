# SPDX-License-Identifier: GPL-3.0-or-later
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
          -Wall -Wextra -Werror -std=c11 -DNNNIX_VERSION='"$(VERSION)"' \
          -DLFS_CONFIG=lfs_port.h -DLFS_NAME_MAX=31 -Ifs -Ithird_party/littlefs
LDFLAGS = -nostdlib -fuse-ld=lld -Wl,-T,arch/nuc140/link.ld,--gc-sections,-Map,build/nnnix.map

SRCS = $(wildcard arch/nuc140/*.c kernel/*.c drivers/*.c lib/*.c net/*.c fs/*.c user/*.c) third_party/littlefs/lfs.c
OBJS = $(SRCS:%.c=build/%.o)

.PHONY: all flash test host-test size clean sdk
all: build/nnnix.bin

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) -MMD -MP -c $< -o $@

# Upstream code is kept unmodified, so its warnings are not errors here.
build/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) -Wno-error -w -MMD -MP -c $< -o $@

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

# Loadable example programs (sdk/), built before the tests upload them.
sdk:
	$(MAKE) -C sdk

test: build/nnnix.bin sdk
	sudo $(PYTHON) tools/nntest.py --flash build/nnnix.bin

HOST_TESTS = build/host/test_lib build/host/test_input build/host/test_kmem build/host/test_tty build/host/test_console build/host/test_shparse build/host/test_http \
             build/host/test_time build/host/test_ed build/host/test_rootfs build/host/test_usbfs
build/host/test_lib: tests/host/test_lib.c lib/fmt.c lib/string.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fno-builtin $^ -o $@
build/host/test_input: tests/host/test_input.c drivers/ch9350.c drivers/hidkey.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST $^ -o $@
build/host/test_kmem: tests/host/test_kmem.c kernel/kmem.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
build/host/test_tty: tests/host/test_tty.c kernel/ledit.c net/telnet.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
build/host/test_console: tests/host/test_console.c kernel/vt.c drivers/kbd.c drivers/hidkey.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
build/host/test_shparse: tests/host/test_shparse.c user/shparse.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
build/host/test_http: tests/host/test_http.c net/http.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
build/host/test_time: tests/host/test_time.c lib/time.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
build/host/test_ed: tests/host/test_ed.c user/edcore.c lib/fmt.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
LFS_HOST = -DLFS_CONFIG=lfs_port.h -DLFS_NAME_MAX=31 -Ifs -Ithird_party/littlefs
build/host/test_rootfs: tests/host/test_rootfs.c fs/rootfs.c fs/lfs_port.c third_party/littlefs/lfs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -g -DNNNIX_HOST $(LFS_HOST) -fsanitize=undefined -fno-sanitize-recover=undefined -c tests/host/test_rootfs.c -o $@.t.o
	$(HOSTCC) -std=c11 -w -g -DNNNIX_HOST $(LFS_HOST) -c fs/rootfs.c -o $@.r.o
	$(HOSTCC) -std=c11 -w -g -DNNNIX_HOST $(LFS_HOST) -c fs/lfs_port.c -o $@.p.o
	$(HOSTCC) -std=c11 -w -g -DNNNIX_HOST $(LFS_HOST) -c third_party/littlefs/lfs.c -o $@.l.o
	$(HOSTCC) -fsanitize=undefined $@.t.o $@.r.o $@.p.o $@.l.o -o $@
build/host/test_usbfs: tests/host/test_usbfs.c fs/usbfs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
host-test: $(HOST_TESTS)
	@for t in $(HOST_TESTS); do echo "== $$t"; (cd tests/host && ../../$$t) || exit 1; done

size: build/nnnix.elf
	$(SIZE) -A $<

clean:
	rm -rf build

-include $(OBJS:.o=.d)
