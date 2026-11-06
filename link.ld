OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x100000;
 .multiboot : { KEEP(*(.multiboot)) } :code
 . = ALIGN(4096);
 __text_start = .;
 .text : { *(.text.boot) *(.text .text.*) } :code
 . = ALIGN(4096);
 __text_end = .;
 .rodata : { *(.rodata .rodata.*) } :code
 . = ALIGN(4096);
 __rodata_end = .;
 .data : { *(.data .data.*) } :data
 . = ALIGN(4096);
 __bss_start = .;
 .bss (NOLOAD) : {
  *(.bss.boot)
  . = ALIGN(4096); *(.bss.idt)
  . = ALIGN(4096); *(.bss.dma)
  *(.bss .bss.*) *(COMMON)
 } :data
 . = ALIGN(4096);
 __kernel_end = .;
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
ASSERT(SIZEOF(.multiboot) == 12, "Multiboot header missing")
ASSERT(__kernel_end < 0x4000000, "Kernel exceeds 64 MiB")
