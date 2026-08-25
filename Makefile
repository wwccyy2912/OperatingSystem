# ==============================================================================
# Makefile - OpSys x86_64 Microkernel Build System
# ==============================================================================

# Toolchain -------------------------------------------------------------------
AS      := nasm
CC      := gcc
OBJCOPY := objcopy

# Prefer ld.lld, fall back to ld
LD := $(shell command -v ld.lld 2>/dev/null || command -v ld)

# GCC built-in freestanding headers (stdint.h, stddef.h, etc.)
# -nostdinc hides system libc; -isystem brings back only GCC's own headers.
FREESTANDING_INC := $(shell $(CC) -print-file-name=include)

# Kernel flags ----------------------------------------------------------------
# Stack protector: kernel builds with -fstack-protector-strong (canary checks
# on functions with local arrays etc.).  The runtime (__stack_chk_guard /
# __stack_chk_fail) is provided by kernel/arch/x86_64/stack_chk.c.
# -mstack-protector-guard=global is REQUIRED: the default guard lives at
# %fs:0x28 (TLS slot), but a freestanding kernel has no FS base set up --
# that would read physical address 0x28 (real-mode IVT area) instead of a
# kernel-owned canary.
CFLAGS  := -ffreestanding -ffunction-sections -fdata-sections \
           -fstack-protector-strong -mstack-protector-guard=global \
           -nostdlib -nostdinc -mcmodel=large \
           -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
           -isystem $(FREESTANDING_INC) \
           -I kernel/include -std=c11 -Wall -Wextra -O2 \
           -MD -MP

ASFLAGS := -f elf64

LDFLAGS := -T kernel/arch/x86_64/linker.ld -nostdlib -z max-page-size=0x1000

# User-space flags (different include paths, large model for position-independent)
USER_CFLAGS := -ffreestanding -ffunction-sections -fdata-sections \
               -fno-stack-protector -nostdlib -nostdinc -mcmodel=large \
               -mno-red-zone \
               -isystem $(FREESTANDING_INC) \
               -I user/lib -I user/lib/libos -I user/lib/libc \
               -I user/runtime/include \
               -I kernel/include \
               -std=c11 -Wall -Wextra -O2 \
               -MD -MP

USER_LDFLAGS := -T scripts/user.ld -nostdlib -z max-page-size=0x1000 \
                --gc-sections -z separate-code

# Source files ----------------------------------------------------------------
KERNEL_ASM := \
    kernel/arch/x86_64/boot.asm \
    kernel/arch/x86_64/context_switch.S \
    kernel/arch/x86_64/syscall_entry.S

KERNEL_C := \
    kernel/arch/x86_64/gdt.c \
    kernel/arch/x86_64/idt.c \
    kernel/arch/x86_64/serial.c \
    kernel/arch/x86_64/io.c \
    kernel/arch/x86_64/rtc.c \
    kernel/arch/x86_64/rng.c \
    kernel/arch/x86_64/virtio_blk.c \
    kernel/arch/x86_64/stack_chk.c \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/elf_boot.c \
    kernel/mm/rbtree.c \
    kernel/mm/vspace.c \
    kernel/mm/shm.c \
    kernel/sched/sched.c \
    kernel/sched/thread.c \
    kernel/sched/thread_ctx.c \
    kernel/process/process.c \
    kernel/process/signal.c \
    kernel/ipc/ipc.c \
    kernel/ipc/notify.c \
    kernel/ipc/irq.c \
    kernel/ipc/mutex.c \
    kernel/cap/cap.c \
    kernel/blob/blob.c \
    kernel/syscall/syscall.c \
    kernel/syscall/process_desc.c \
    kernel/syscall/pci.c \
    kernel/gfx/framebuffer.c \
    kernel/panic.c \
    kernel/kernel_main.c

USER_C := \
    user/runtime/init.c \
    user/runtime/exit.c \
    user/runtime/errno.c \
    user/runtime/malloc.c \
    user/runtime/signal_user.c \
    user/lib/libc/stdio.c \
    user/lib/libc/stdlib.c \
    user/lib/libc/string.c \
    user/lib/libc/ctype.c \
    user/lib/libc/inttypes.c \
    user/lib/libc/time.c \
    user/lib/libc/math.c \
    user/lib/libc/threads.c \
    user/lib/libc/wchar.c \
    user/lib/libc/wctype.c \
    user/lib/libos/syscalls.c \
    user/lib/libos/elf_parse.c \
    user/lib/libipc/ipc.c \
    user/services/init/main.c \
    user/services/manager/manager.c \
    user/services/shell/shell.c \
    user/services/serial/serial.c \
    user/services/keyboard/keyboard.c \
    user/services/term/term.c \
    user/services/flaky/main.c \
    user/services/crashpeer/main.c \
    user/services/hello/main.c \
    user/services/vfs/vfs_server.c \
    user/services/vfs/fs_mem_driver.c \
    user/services/vfs/fs_virtio_blk_driver.c \
    user/services/perm/perm-manager.c \
    user/services/device_mgr/device_mgr.c \
    user/services/pkg/pkg_manager.c \
    user/services/sbox_demo/main.c \
    user/services/runtime_demo/main.c \
    user/services/tui_demo/main.c \
    user/services/window_demo/main.c \
    user/services/user/main.c \
    user/services/wm/main.c \
    user/services/wm_demo/main.c \
    user/services/policy/main.c \
    user/lib/libtui/tui.c \
    user/lib/libwm/wm.c \
    user/lib/libfs/fs.c \
    user/lib/libpkg/pkg.c

# Object files (mirror source tree under build/) ------------------------------
# NASM .asm -> build/<path>.asm.o   NASM .S -> build/<path>.S.o
KERNEL_ASM_OBJ := $(patsubst %.asm, build/%.asm.o, $(filter %.asm, $(KERNEL_ASM))) \
                  $(patsubst %.S, build/%.S.o, $(filter %.S, $(KERNEL_ASM)))
KERNEL_C_OBJ  := $(patsubst %.c, build/%.c.o, $(KERNEL_C))
KERNEL_OBJ    := $(KERNEL_ASM_OBJ) $(KERNEL_C_OBJ)

USER_ASM := \
    user/runtime/crt0.S \
    user/lib/libc/setjmp.S
USER_C_OBJ   := $(patsubst %.c, build/%.c.o, $(USER_C))
USER_ASM_OBJ := $(patsubst %.S, build/%.S.o, $(USER_ASM))
USER_OBJ     := $(USER_C_OBJ) $(USER_ASM_OBJ)

# --- User service process images ---------------------------------------------
# Every service (init, manager, serial, shell, flaky, hello) is an independent
# user process.  Each ELF links the shared user objects (runtime, libc, libos,
# libipc, crt0) PLUS its own entry object, and EXCLUDES the entry objects of
# all other services (they define their own main()).  Each ELF is embedded
# into kernel.elf as a blob so any process can fetch it via SYS_BLOB_GET and
# spawn it via SYS_PROCESS_CREATE.
USER_SVC_ENTRY_OBJ := \
    build/user/services/init/main.c.o \
    build/user/services/manager/manager.c.o \
    build/user/services/serial/serial.c.o \
    build/user/services/keyboard/keyboard.c.o \
    build/user/services/term/term.c.o \
    build/user/services/shell/shell.c.o \
    build/user/services/flaky/main.c.o \
    build/user/services/crashpeer/main.c.o \
    build/user/services/hello/main.c.o \
    build/user/services/vfs/vfs_server.c.o \
    build/user/services/vfs/fs_mem_driver.c.o \
    build/user/services/vfs/fs_virtio_blk_driver.c.o \
    build/user/services/perm/perm-manager.c.o \
    build/user/services/device_mgr/device_mgr.c.o \
    build/user/services/pkg/pkg_manager.c.o \
    build/user/services/sbox_demo/main.c.o \
    build/user/services/runtime_demo/main.c.o \
    build/user/services/tui_demo/main.c.o \
    build/user/services/window_demo/main.c.o \
    build/user/services/user/main.c.o \
    build/user/services/wm/main.c.o \
    build/user/services/wm_demo/main.c.o \
    build/user/services/policy/main.c.o
USER_SHARED_OBJ := $(filter-out $(USER_SVC_ENTRY_OBJ), $(USER_OBJ))

SVC_NAMES := init manager serial keyboard term shell flaky crashpeer hello vfs fs_mem_driver fs_virtio_blk_driver perm device_mgr pkg sbox_demo runtime_demo tui_demo window_demo user wm wm_demo policy
SVC_BLOBS := $(addprefix build/, $(addsuffix _blob.o, $(SVC_NAMES)))

# ==============================================================================
# Targets
# ==============================================================================

.PHONY: all kernel.elf init_user iso run debug clean help

all: kernel.elf

help:
	@echo "OpSys Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all         Build kernel.elf (default)"
	@echo "  init_user   Build user-space init binary"
	@echo "  iso         Create bootable ISO with GRUB"
	@echo "  run         Build ISO and run in QEMU"
	@echo "  debug       Build ISO and run in QEMU with GDB stub"
	@echo "  clean       Remove build directory"
	@echo "  help        Show this help"
	@echo ""
	@echo "Toolchain:"
	@echo "  CC=$(CC)  AS=$(AS)  LD=$(LD)  OBJCOPY=$(OBJCOPY)"

# --- Kernel -------------------------------------------------------------------
# Link kernel; include init_blob.o and hello_blob.o (kernel-side ELF blobs)
kernel.elf: $(KERNEL_OBJ) $(SVC_BLOBS)
	$(LD) $(LDFLAGS) -o $@ $^

# Kernel assembly - both .asm and .S are NASM (Intel syntax, 64-bit ELF)
build/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

build/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Kernel C (uses kernel CFLAGS with -mcmodel=kernel)
build/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# --- User-space ---------------------------------------------------------------
# Compile user-space, link each service at 0x400000, embed ELF as blob
init_user: build/user/services/init.elf

# Link each service ELF: shared user objects + the service's own entry object.
# NOTE: one rule per service — a single shared recipe under multiple target
# lines would only attach to the LAST target in GNU make.
define SVC_LINK_RULE
build/user/services/$(1).elf: build/user/services/$(2) $(USER_SHARED_OBJ)
	@mkdir -p $$(dir $$@)
	$(LD) $(USER_LDFLAGS) -o $$@ $$^
endef
$(eval $(call SVC_LINK_RULE,init,init/main.c.o))
$(eval $(call SVC_LINK_RULE,manager,manager/manager.c.o))
$(eval $(call SVC_LINK_RULE,serial,serial/serial.c.o))
$(eval $(call SVC_LINK_RULE,keyboard,keyboard/keyboard.c.o))
$(eval $(call SVC_LINK_RULE,term,term/term.c.o))
$(eval $(call SVC_LINK_RULE,shell,shell/shell.c.o))
$(eval $(call SVC_LINK_RULE,flaky,flaky/main.c.o))
$(eval $(call SVC_LINK_RULE,crashpeer,crashpeer/main.c.o))
$(eval $(call SVC_LINK_RULE,hello,hello/main.c.o))
$(eval $(call SVC_LINK_RULE,vfs,vfs/vfs_server.c.o))
$(eval $(call SVC_LINK_RULE,fs_mem_driver,vfs/fs_mem_driver.c.o))
$(eval $(call SVC_LINK_RULE,fs_virtio_blk_driver,vfs/fs_virtio_blk_driver.c.o))
$(eval $(call SVC_LINK_RULE,perm,perm/perm-manager.c.o))
$(eval $(call SVC_LINK_RULE,device_mgr,device_mgr/device_mgr.c.o))
$(eval $(call SVC_LINK_RULE,pkg,pkg/pkg_manager.c.o))
$(eval $(call SVC_LINK_RULE,sbox_demo,sbox_demo/main.c.o))
$(eval $(call SVC_LINK_RULE,runtime_demo,runtime_demo/main.c.o))
$(eval $(call SVC_LINK_RULE,tui_demo,tui_demo/main.c.o))
$(eval $(call SVC_LINK_RULE,window_demo,window_demo/main.c.o))
$(eval $(call SVC_LINK_RULE,user,user/main.c.o))
$(eval $(call SVC_LINK_RULE,wm,wm/main.c.o))
$(eval $(call SVC_LINK_RULE,wm_demo,wm_demo/main.c.o))
$(eval $(call SVC_LINK_RULE,policy,policy/main.c.o))

# Embed each service ELF as a kernel blob object (symbols renamed to <svc>_elf_*).
define SVC_BLOB_RULE
build/$(1)_blob.o: build/user/services/$(1).elf
	@mkdir -p $$(dir $$@)
	cd build && ld -r -b binary -o $(1)_blob_raw.o user/services/$(1).elf
	objcopy --redefine-sym _binary_user_services_$(1)_elf_start=$(1)_elf_start \
	        --redefine-sym _binary_user_services_$(1)_elf_end=$(1)_elf_end \
	        --redefine-sym _binary_user_services_$(1)_elf_size=$(1)_elf_size \
	        build/$(1)_blob_raw.o $$@
	rm -f build/$(1)_blob_raw.o
endef
$(foreach svc,$(SVC_NAMES),$(eval $(call SVC_BLOB_RULE,$(svc))))

# User C files (different CFLAGS: -mcmodel=large, different include paths)
$(USER_C_OBJ): build/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# User assembly files (NASM)
$(USER_ASM_OBJ): build/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# --- ISO image ----------------------------------------------------------------
iso: kernel.elf init_user
	mkdir -p build/isodir/boot/grub
	cp kernel.elf build/isodir/boot/kernel.elf
	cp boot/grub.cfg build/isodir/boot/grub/grub.cfg
	grub2-mkrescue --modules="all_video gfxterm gfxterm_background font" \
		--locales="" -o build/opsos.iso build/isodir 2>/dev/null

# --- QEMU ---------------------------------------------------------------------
run: iso
	qemu-system-x86_64 \
		-cdrom build/opsos.iso \
		-m 256M \
		-serial stdio \
		-d int,cpu_reset,guest_errors \
		-drive file=disk.img,if=none,id=vd,cache=writethrough \
		-device virtio-blk-pci,drive=vd,disable-modern=on

debug: iso
	@echo ">>> GDB stub listening on port 1234"
	@echo ">>> Connect with:  gdb kernel.elf -ex 'target remote :1234'"
	qemu-system-x86_64 \
		-cdrom build/opsos.iso \
		-m 256M \
		-nographic \
		-serial mon:stdio \
		-s -S \
		-drive file=disk.img,if=none,id=vd,cache=writethrough \
		-device virtio-blk-pci,drive=vd,disable-modern=on

# --- Cleanup ------------------------------------------------------------------
clean:
	rm -rf build

# --- Code formatting ----------------------------------------------------------
# clang-format enforces the project style (see .clang-format):
#   4-space indent, K&R braces, line width <= 100.
# Excluded: fs_mem_driver.c (frozen), font.h / panic_font.h (generated data).
format:
	@echo ">>> Formatting C source files..."
	@find kernel/ user/ -name '*.c' -o -name '*.h' \
		| grep -v 'fs_mem_driver.c' \
		| grep -v 'term/font.h' \
		| grep -v 'kernel/panic_font.h' \
		| xargs clang-format -i
	@echo ">>> Done. Verify with: make iso"

format-check:
	@echo ">>> Checking formatting..."
	@find kernel/ user/ -name '*.c' -o -name '*.h' \
		| grep -v 'fs_mem_driver.c' \
		| grep -v 'term/font.h' \
		| grep -v 'kernel/panic_font.h' \
		| xargs clang-format --dry-run -Werror
	@echo ">>> All files properly formatted."

# --- Auto-dependency tracking -------------------------------------------------
-include $(KERNEL_C_OBJ:.o=.d) $(USER_OBJ:.o=.d)
