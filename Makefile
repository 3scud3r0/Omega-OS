CC = gcc
OBJCOPY = objcopy
QEMU = qemu-system-x86_64
QEMU_IMG = qemu-img
CFLAGS = -std=c99 -ffreestanding -O3 -flto -fno-stack-protector -fno-pie -no-pie -mno-red-zone -Wall -Wextra -Werror -pedantic -Iinclude
CFLAGS += -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables -mgeneral-regs-only -MMD -MP
SOURCES = $(wildcard src/*.c)
OBJECTS = $(SOURCES:src/%.c=build/%.o)
.PHONY: all clean run test test-host bundle
all: omega_os.elf disk.img
build:
	mkdir -p $@
build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@
build/omega64.elf: $(OBJECTS) link.ld
	$(CC) $(CFLAGS) -nostdlib -Wl,-T,link.ld -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 $(OBJECTS) -o $@
# QEMU's Multiboot loader requires an ELF32 container; code switches to long mode.
omega_os.elf: build/omega64.elf
	$(OBJCOPY) -O elf32-i386 $< $@
disk.img:
	@if command -v $(QEMU_IMG) >/dev/null 2>&1; then $(QEMU_IMG) create -f raw $@ 32M; else python3 -c "with open('$@', 'xb') as f: f.truncate(32*1024*1024)"; fi
run: omega_os.elf disk.img
	$(QEMU) -kernel omega_os.elf -drive file=disk.img,if=virtio,format=raw -smp 2 -m 128M -serial stdio -display none
test: omega_os.elf
	python3 tests/v1_validation.py "$(QEMU)"
test-host: | build
	python3 tests/rational_test.py
	$(CC) -std=c99 -O3 -Wall -Wextra -Werror -pedantic -Iinclude -pthread tests/ipc_stress.c src/ipc.c -o build/ipc_stress
	./build/ipc_stress
bundle:
	python3 tests/bundle_sources.py
clean:
	rm -rf build omega_os.elf
-include $(OBJECTS:.o=.d)
