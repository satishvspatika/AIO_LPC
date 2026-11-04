MEMORY
{
  FLASH_STUB (rx) : ORIGIN = 0x00000000, LENGTH = 0x1000
}
SECTIONS
{
  .vectors : { KEEP(*(.vectors)) } > FLASH_STUB
  .text : { *(.text*) } > FLASH_STUB
}
