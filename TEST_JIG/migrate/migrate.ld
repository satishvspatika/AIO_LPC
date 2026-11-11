/* Migration image: loaded by an old unit's SD bootloader V1.1 as fw.hex into 0x18000 (slot A).
 * That bootloader stops at 0x40000, so the image must stay under 160 KB; slot A is 128 KB anyway.
 * RAM stops short of the top 32 bytes, which the flash-programming ROM (IAP) uses. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00018000, LENGTH = 0x20000
  RAM   (rwx) : ORIGIN = 0x10000000, LENGTH = 0x7FE0
  AHB   (rwx) : ORIGIN = 0x2007C000, LENGTH = 0x8000
}
ENTRY(Reset_Handler)
__stack_top = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS
{
  .text : ALIGN(4)
  {
    KEEP(*(.isr_vector))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  __data_load = LOADADDR(.data);
  .data : ALIGN(4) { __data_start = .; *(.data*) . = ALIGN(4); __data_end = .; } > RAM AT > FLASH
  .bss (NOLOAD) : ALIGN(4) { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
  /* Big buffers that never go to the flash ROM as a source: AHB SRAM, not zeroed */
  .ahb (NOLOAD) : ALIGN(4) { *(.ahb*) } > AHB
  /DISCARD/ : { *(.comment) }
}
ASSERT(__bss_end <= __stack_top - 0x1000, "migration image: less than 4 KB left for the stack")
