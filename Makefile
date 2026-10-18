# SPDX-License-Identifier: GPL-3.0-or-later
# nnnix build. Targets:
#   make            build build/nnnix.elf and build/nnnix.bin, and the programs
#                   for /bin (build/bin/*.nxe, sources in bin/)
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

.PHONY: all flash test host-test size clean sdk boot programs
all: build/nnnix.bin programs

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

# Programs for /bin, built like the SDK's (sdk/Makefile: -fropi -frwpi, run
# from APROM by kernel/xip.c) from the commands in bin/: bin/main.c calls
# cmd_NAME. tools/nnupdate.py --bin build/bin installs them.
PROGRAMS = play ed wc head tail grep hexdump
PROG_SRCS_play = bin/play.c
PROG_SRCS_ed = bin/ed.c bin/edcore.c
PROG_SRCS_wc = bin/textutil.c
PROG_SRCS_head = bin/textutil.c
PROG_SRCS_tail = bin/textutil.c
PROG_SRCS_grep = bin/textutil.c
PROG_SRCS_hexdump = bin/textutil.c
PROG_CFLAGS = --target=arm-none-eabi -mcpu=cortex-m0 -mthumb -Os -ffreestanding -fno-builtin \
              -ffunction-sections -fdata-sections -fropi -frwpi -Wall -Wextra -Werror -std=c11 -Isdk/include -Ilib
PROG_LDFLAGS = -nostdlib -fuse-ld=lld -Wl,-T,sdk/nxe.ld,--emit-relocs,--gc-sections

programs: $(PROGRAMS:%=build/bin/%.nxe)
build/prog/%.o: bin/%.c
	@mkdir -p $(dir $@)
	$(CLANG) $(PROG_CFLAGS) -MMD -MP -c $< -o $@
build/prog/crt0.o: sdk/crt0.c sdk/include/nnnix.h
	@mkdir -p $(dir $@)
	$(CLANG) $(PROG_CFLAGS) -c $< -o $@
build/prog/main_%.o: bin/main.c
	@mkdir -p $(dir $@)
	$(CLANG) $(PROG_CFLAGS) -DMAIN=cmd_$* -c $< -o $@
prog_objs = $(patsubst bin/%.c,build/prog/%.o,$(PROG_SRCS_$1))
.SECONDEXPANSION:
build/bin/%.elf: build/prog/crt0.o build/prog/main_%.o $$(call prog_objs,$$*) sdk/nxe.ld
	@mkdir -p $(dir $@)
	$(CLANG) $(PROG_CFLAGS) $(PROG_LDFLAGS) $(filter %.o,$^) -o $@
build/bin/%.nxe: build/bin/%.elf tools/mknxe.py
	$(PYTHON) tools/mknxe.py $< $@
.SECONDARY: $(PROGRAMS:%=build/bin/%.elf) $(PROGRAMS:%=build/prog/main_%.o) $(patsubst bin/%.c,build/prog/%.o,$(wildcard bin/*.c))

# LDROM bootloader (boot/): at most 4 KiB.
BOOT_OBJS = build/boot/boot.o build/boot/update.o
build/boot/%.o: boot/%.c lib/fwupdate.h boot/update.h build/.version
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) -DBOOT_VERSION='"$(VERSION)"' -c $< -o $@
build/boot.elf: $(BOOT_OBJS) boot/link.ld
	$(CLANG) $(CFLAGS) -nostdlib -fuse-ld=lld -Wl,-T,boot/link.ld,--gc-sections $(BOOT_OBJS) -o $@
build/boot.bin: build/boot.elf
	$(OBJCOPY) -O binary $< $@
	@test $$(stat -c %s $@) -le 4096 || { echo "boot.bin is larger than LDROM (4 KiB)"; exit 1; }
	@echo "boot.bin: $$(stat -c %s $@) bytes of 4096"
boot: build/boot.bin

# Loadable example programs (sdk/), built before the tests upload them.
sdk:
	$(MAKE) -C sdk

test: build/nnnix.bin programs sdk
	sudo $(PYTHON) tools/nntest.py --flash build/nnnix.bin

HOST_TESTS = build/host/test_lib build/host/test_input build/host/test_kmem build/host/test_tty build/host/test_console build/host/test_shparse build/host/test_http \
             build/host/test_time build/host/test_ed build/host/test_rootfs build/host/test_usbfs build/host/test_boot build/host/test_nnv build/host/test_xip build/host/test_scrollback
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
build/host/test_scrollback: tests/host/test_scrollback.c kernel/scrollback.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
build/host/test_xip: tests/host/test_xip.c kernel/xip.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
build/host/test_ed: tests/host/test_ed.c bin/edcore.c lib/fmt.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $^ -o $@
build/host/test_boot: tests/host/test_boot.c boot/update.c
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
NNV_DEMOS = pattern bounce mixed tone
build/host/nnv_%.nnv: tools/mknnv.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mknnv.py demo $* $@ --raw build/host/nnv_$*.raw
build/host/test_nnv: tests/host/test_nnv.c lib/nnv.h $(NNV_DEMOS:%=build/host/nnv_%.nnv)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -g -DNNNIX_HOST -fsanitize=undefined -fno-sanitize-recover=undefined $< -o $@
host-test: $(HOST_TESTS)
	@for t in $(HOST_TESTS); do echo "== $$t"; (cd tests/host && ../../$$t) || exit 1; done

size: build/nnnix.elf
	$(SIZE) -A $<

clean:
	rm -rf build

-include $(OBJS:.o=.d) $(wildcard build/prog/*.d)
