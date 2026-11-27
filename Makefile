CC      := gcc
AS      := gcc
LD      := ld

CFLAGS  := -m32 -std=gnu11 -ffreestanding -O2 -Wall -Wextra \
           -fno-stack-protector -fno-pie -fno-pic -fno-builtin \
           -fno-tree-loop-distribute-patterns \
           -Iusr/include -Iusr/include/kernel -MMD -MP \
           -mno-sse -mno-sse2 -mno-mmx -mno-80387 -march=i386 \
	   -mtune=i386 -fcf-protection=none -fno-pie -fno-stack-protector
ASFLAGS := -m32 -c
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

USER_CFLAGS := -m32 -std=gnu11 -ffreestanding -nostdinc -O2 -Wall -Wextra \
               -fno-stack-protector -fno-pie -fno-pic -fno-builtin \
               -fno-tree-loop-distribute-patterns -march=i386 \
               -mno-sse -mno-sse2 -mno-mmx -mno-80387 -Iusr/include -Iuser/ \
               -fcf-protection=none -fno-pie -fno-stack-protector -mtune=i386
USER_LDFLAGS := -m elf_i386 -T user/user.ld -nostdlib \
                -z max-page-size=4096 -z noexecstack

USER_BINS := rootfs/bin/echo rootfs/bin/sh \
             rootfs/bin/ls rootfs/bin/cat rootfs/bin/whoami \
             rootfs/bin/id rootfs/bin/chmod rootfs/bin/chown rootfs/bin/sha256 \
             rootfs/bin/cp rootfs/bin/mv rootfs/bin/rm rootfs/bin/mkdir \
	     rootfs/bin/touch rootfs/bin/grep rootfs/bin/uname \
             rootfs/bin/vi rootfs/bin/clear rootfs/bin/sleep rootfs/bin/kill \
             rootfs/bin/pkill rootfs/bin/ps rootfs/bin/wc rootfs/bin/head \
             rootfs/bin/tail rootfs/bin/tee rootfs/bin/true rootfs/bin/false \
             rootfs/bin/sync rootfs/bin/login rootfs/bin/umount rootfs/bin/mount \
             rootfs/sbin/init rootfs/sbin/halt rootfs/sbin/poweroff \
             rootfs/sbin/reboot rootfs/bin/runlevel rootfs/bin/telinit \
             rootfs/sbin/diskd

OBJS := boot/boot.o boot/gdt.o boot/isr.o boot/switch.o \
        kernel/kernel.o kernel/terminal.o kernel/kprintf.o kernel/string.o \
        kernel/gdt.o kernel/idt.o kernel/pic.o kernel/pit.o kernel/keyboard.o \
        kernel/pmm.o kernel/paging.o kernel/kheap.o kernel/floppy.o kernel/part.o \
        kernel/vfs.o kernel/ramfs.o kernel/devfs.o kernel/pipe.o kernel/ext2.o \
        kernel/syscall.o kernel/elf.o kernel/proc.o kernel/tty.o kernel/power.o kernel/wait.o \
        kernel/procfs.o
KERNEL := usr-ux-2.bin

all: $(KERNEL)

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@grub-file --is-x86-multiboot $@ && echo "Multiboot OK"

boot/%.o: boot/%.S
	$(AS) $(ASFLAGS) $< -o $@

kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

user/crt0.o: user/crt0.s
	$(AS) $(ASFLAGS) $< -o $@

user/%.o: user/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

rootfs/sbin/%: user/%.o user/crt0.o user/user.ld
	mkdir -p rootfs/sbin
	$(LD) $(USER_LDFLAGS) -o $@ user/crt0.o $<

rootfs/bin/%: user/%.o user/crt0.o user/user.ld
	mkdir -p rootfs/bin
	$(LD) $(USER_LDFLAGS) -o $@ user/crt0.o $<

rootfs/sbin/init: user/init.o user/crt0.o user/user.ld
	mkdir -p rootfs/sbin
	$(LD) $(USER_LDFLAGS) -o $@ user/crt0.o $<


run: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL) -drive file=disk.img,format=raw,if=ide

disk: $(KERNEL) $(USER_BINS)
	sh tools/mkimage.sh

iso: $(KERNEL)
	mkdir -p isodir/boot/grub
	cp $(KERNEL) isodir/boot/$(KERNEL)
	printf 'menuentry "USR/UX 2" {\n\tmultiboot /boot/$(KERNEL)\n}\n' > isodir/boot/grub/grub.cfg
	grub-mkrescue -o usr-ux-2.iso isodir

bootsys:
	echo Please use 86box instead. QEMU floppy probably doesnt work right now.
	qemu-system-i386 -fda floppy.img

clean:
	rm -rf $(OBJS) $(OBJS:.o=.d) $(KERNEL) isodir usr-ux-2.iso user/*.o rootfs/bin/ rootfs/sbin/ /rootfs/boot/kernel.bin

.PHONY: all run iso clean

-include $(OBJS:.o=.d)
