ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : { *(.text.entry) *(.text .text.*) *(.rodata .rodata.*) }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) *(.dyn*) *(.interp) *(.hash) }
}
