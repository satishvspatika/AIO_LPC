/* LPC1768 bootloader: sectors 2-11. Its RAM stops short of the top 32 bytes, which IAP uses. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00002000, LENGTH = 0xA000
  RAM   (rwx) : ORIGIN = 0x10000000, LENGTH = 0x7FE0
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
ASSERT(SIZEOF(.bss) < 0x4000, "bootloader RAM too large")
