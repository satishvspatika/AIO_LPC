/* LPC845 firmware, linked to run in slot A under the bootloader. The MCUXpresso-generated scripts in
 * Debug stay as they were, for reference. */
INCLUDE "lpcxpresso845max_gpio_led_output_Debug_library.ld"

MEMORY
{
  MFlash64 (rx)  : ORIGIN = 0x00002400, LENGTH = 0x6000
  RamLoc16 (rwx) : ORIGIN = 0x10000000, LENGTH = 0x4000
}
__top_RamLoc16 = 0x10004000;
/* The chip's flash routines use the top 32 bytes of RAM, so the stack starts below them */
__user_stack_top = 0x10003FE0;

ENTRY(ResetISR)

SECTIONS
{
    .text : ALIGN(4)
    {
        FILL(0xff)
        __vectors_start__ = ABSOLUTE(.) ;
        KEEP(*(.isr_vector))
        KEEP(*(.image_info))
        . = ALIGN(4) ;
        __section_table_start = .;
        __data_section_table = .;
        LONG(LOADADDR(.data));
        LONG(    ADDR(.data));
        LONG(  SIZEOF(.data));
        __data_section_table_end = .;
        __bss_section_table = .;
        LONG(    ADDR(.bss));
        LONG(  SIZEOF(.bss));
        __bss_section_table_end = .;
        __section_table_end = . ;
        *(.after_vectors*)
        *(.text*)
        *(.rodata .rodata.* .constdata .constdata.*)
        . = ALIGN(4);
    } > MFlash64

    .ARM.extab : ALIGN(4) { *(.ARM.extab* .gnu.linkonce.armextab.*) } > MFlash64
    __exidx_start = .;
    .ARM.exidx : ALIGN(4) { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > MFlash64
    __exidx_end = .;
    _etext = .;

    .data : ALIGN(4)
    {
       FILL(0xff)
       _data = . ;
       *(vtable)
       *(.ramfunc*)
       *(.data*)
       . = ALIGN(4) ;
       _edata = . ;
    } > RamLoc16 AT>MFlash64

    .bss :
    {
        . = ALIGN(4) ;
        _bss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4) ;
        _ebss = .;
        PROVIDE(end = .);
    } > RamLoc16

    /* Fixed address from 845-v1.5, so the rain count kept here is found by every later version
     * after an update restart. Variables (.data + .bss) must stay below it. */
    .noinit 0x10002800 (NOLOAD):
    {
         . = ALIGN(4) ;
        *(.noinit*)
         . = ALIGN(4) ;
        _noinit_end = . ;
    } > RamLoc16
    ASSERT(_ebss <= 0x10002800, "LPC845 variables overlap the kept rain count at 0x10002800")

    /* No code protection, which also keeps a stray value out of this word */
    /DISCARD/ : { *(.crp) }

    PROVIDE(_pvHeapStart = DEFINED(__user_heap_base) ? __user_heap_base : .);
    PROVIDE(_vStackTop = __user_stack_top);
    PROVIDE(__valid_user_code_checksum = 0 -
                                         (_vStackTop
                                         + (ResetISR + 1)
                                         + (( DEFINED(NMI_Handler) ? NMI_Handler : M0_NMI_Handler ) + 1)
                                         + (( DEFINED(HardFault_Handler) ? HardFault_Handler : M0_HardFault_Handler ) + 1)
                                         )
           );
    _image_start = LOADADDR(.text);
    _image_end = LOADADDR(.data) + SIZEOF(.data);
    _image_size = _image_end - _image_start;
}
