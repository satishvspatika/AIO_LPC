/* LPC845 bootloader: sectors 0-3. Its RAM stops short of the top 32 bytes, which the flash routines use. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 0x1000
  RAM   (rwx) : ORIGIN = 0x10000000, LENGTH = 0x3FE0
}
ENTRY(Reset_Handler)
__stack_top = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS
{
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  __data_load = LOADADDR(.data);
  .data : { __data_start = .; *(.data*) . = ALIGN(4); __data_end = .; } > RAM AT > FLASH
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
  /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
