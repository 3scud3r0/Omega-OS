OUTPUT_FORMAT(binary)
ENTRY(_start)
SECTIONS {
 . = 0x80000000;
 .text : { *(.text .text.*) }
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data .data.*) }
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
ASSERT(. <= 0x80001000, "agent exceeds code page")
