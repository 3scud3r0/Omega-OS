CC = gcc
OBJCOPY = objcopy
QEMU = qemu-system-x86_64
QEMU_IMG = qemu-img
DISK ?= disk.img
CFLAGS = -std=c99 -ffreestanding -O3 -flto -fno-stack-protector -fno-pie -no-pie -mno-red-zone -Wall -Wextra -Werror -pedantic -Iinclude -Iinclude/crypto -Ibuild
CFLAGS += -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables -mgeneral-regs-only -MMD -MP
SOURCES = $(wildcard src/*.c) $(wildcard src/crypto/*.c)
OBJECTS = $(SOURCES:src/%.c=build/%.o)
.PHONY: all clean run test test-host bundle
all: omega_os.elf $(DISK)
build:
	mkdir -p $@
build/%.o: src/%.c | build
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
USERFLAGS = -mcmodel=large -std=c99 -ffreestanding -fno-builtin -O2 -fno-stack-protector -fno-pie -mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wall -Wextra -Werror -pedantic -Iinclude -Iuser/omega
build/omega_generated.c: tests/bootstrap_omega.py $(shell find vendor/omega -type f) | build
	python3 tests/bootstrap_omega.py
build/omega_generated.o: build/omega_generated.c user/omega/omega_value.h
	$(CC) $(USERFLAGS) -c $< -o $@
build/omega_driver.o: user/omega/driver.c user/omega/omega_value.h | build
	$(CC) $(USERFLAGS) -c $< -o $@
build/omega_rational.o: src/rational.c | build
	$(CC) $(USERFLAGS) -c $< -o $@
build/omega_bytecode.o: src/bytecode.c | build
	$(CC) $(USERFLAGS) -c $< -o $@
build/compiler.elf: build/omega_generated.o build/omega_driver.o build/omega_rational.o build/omega_bytecode.o user/omega/link.ld
	$(LD) --gc-sections -T user/omega/link.ld -o $@ $(filter %.o,$^)
build/agent.o: user/agent.c | build
	$(CC) -std=c99 -ffreestanding -fno-builtin -O2 -fno-stack-protector -fno-pie -mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -pedantic -c $< -o $@
build/agent.bin: build/agent.o user/agent.ld
	$(LD) -T user/agent.ld $< -o $@
ifneq ($(AUTHORITY_BUNDLE),)
build/capsule.h: $(AUTHORITY_BUNDLE) | build
	cp $< $@
else
build/capsule.h: build/agent.bin build/compiler.elf tests/make_capsule.py
	python3 tests/make_capsule.py
endif
build/admission.o: build/capsule.h
build/omega64.elf: $(OBJECTS) link.ld
	$(CC) $(CFLAGS) -nostdlib -Wl,-T,link.ld -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 $(OBJECTS) -o $@
# QEMU's Multiboot loader requires an ELF32 container; code switches to long mode.
omega_os.elf: build/omega64.elf
	$(OBJCOPY) -O elf32-i386 $< $@
$(DISK):
	@if command -v $(QEMU_IMG) >/dev/null 2>&1; then $(QEMU_IMG) create -f raw $@ 32M; else python3 -c "with open('$@', 'xb') as f: f.truncate(32*1024*1024)"; fi
run: omega_os.elf $(DISK)
	$(QEMU) -kernel omega_os.elf -drive file=$(DISK),if=virtio,format=raw -smp 2 -m 128M -serial stdio -display none
test: omega_os.elf
	python3 tests/v3_validation.py "$(QEMU)"
	python3 tests/corruption_test.py "$(QEMU)"
	python3 tests/v3_network.py "$(QEMU)"
	python3 tests/format_test.py "$(QEMU)"
test-host: build/capsule.h | build
	python3 tests/tunnel_test.py
	python3 tests/bytecode_test.py
	python3 tests/admission_test.py
	python3 tests/rational_test.py
	$(CC) -std=c99 -O3 -Wall -Wextra -Werror -pedantic -Iinclude -pthread tests/ipc_stress.c src/ipc.c -o build/ipc_stress
	./build/ipc_stress
bundle:
	python3 tests/bundle_sources.py
clean:
	rm -rf build omega_os.elf
-include $(OBJECTS:.o=.d)
