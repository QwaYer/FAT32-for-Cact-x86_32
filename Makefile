KERN_ROOT ?= $(abspath ../CactKernel-x86_32)
LOCAL_REPO ?= $(abspath ../LocalRepoCactOS-x86_32)

_ACTIVE := $(filter-out clean,$(or $(MAKECMDGOALS),all))
ifneq ($(_ACTIVE),)
ifndef KERN_ROOT
$(error KERN_ROOT is required — path to kernel sources with Cact/ headers)
endif
ifndef LOCAL_REPO
$(error LOCAL_REPO is required — directory whose lib/ receives *.cctk)
endif
endif

INSTALL_DIR := $(LOCAL_REPO)/lib

MOD_CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
	-I$(KERN_ROOT)/Cact/kernel/core \
	-I$(KERN_ROOT)/Cact/kernel/memory \
	-I$(KERN_ROOT)/Cact/kernel/cpudev \
	-I$(KERN_ROOT)/Cact/drivers/pci \
	-I$(KERN_ROOT)/Cact/drivers/block/blkdev \
	-I$(KERN_ROOT)/Cact/drivers/block/pagecache \
	-I$(KERN_ROOT)/Cact/fs/vfs \
	-I. \
	-Wall -O2

F32_SRCS := fat32_fat.c fat32_alloc.c fat32_dir.c fat32_vfs.c fat32_write.c fat32_mod.c
F32_OBJS := $(F32_SRCS:.c=.o)

.PHONY: all install clean test

all: fat32.cctk

fat32.cctk: $(F32_OBJS)
	ld -m elf_i386 -r -o $@ $(F32_OBJS)

%.o: %.c fat32.h fat32_internal.h
	gcc $(MOD_CFLAGS) -c $< -o $@

install: fat32.cctk
	@mkdir -p $(INSTALL_DIR)
	cp -f fat32.cctk $(INSTALL_DIR)/fat32.cctk
	@echo "installed: $(INSTALL_DIR)/fat32.cctk"

clean:
	rm -f $(F32_OBJS) fat32.cctk
	rm -rf test/build test/test_fat32

# Host-side test binary (drives the module against a file-backed image).
TEST_CFLAGS := -m32 -fno-pie -fno-builtin -I. \
	-I$(KERN_ROOT)/Cact/kernel/core \
	-I$(KERN_ROOT)/Cact/kernel/memory \
	-I$(KERN_ROOT)/Cact/kernel/cpudev \
	-I$(KERN_ROOT)/Cact/drivers/pci \
	-I$(KERN_ROOT)/Cact/drivers/block/blkdev \
	-I$(KERN_ROOT)/Cact/drivers/block/pagecache \
	-I$(KERN_ROOT)/Cact/fs/vfs \
	-Wall -O0 -g

TEST_OBJS := test/build/fat32_fat.o test/build/fat32_alloc.o \
	test/build/fat32_dir.o test/build/fat32_vfs.o \
	test/build/fat32_write.o test/build/fat32_mod.o \
	test/build/test_fat32.o

test: $(TEST_OBJS)
	gcc -m32 -no-pie -o test/test_fat32 $(TEST_OBJS)

test/build/%.o: %.c fat32.h fat32_internal.h
	@mkdir -p test/build
	gcc $(TEST_CFLAGS) -c $< -o $@

test/build/test_fat32.o: test/test_fat32.c fat32.h fat32_internal.h
	@mkdir -p test/build
	gcc $(TEST_CFLAGS) -c $< -o $@
