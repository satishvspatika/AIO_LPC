/* Memory map for LPC845: limits match the earlier MCUXpresso build */
MEMORY
{
  MFlash64 (rx)  : ORIGIN = 0x00000000, LENGTH = 0xF000
  RamLoc16 (rwx) : ORIGIN = 0x10000000, LENGTH = 0x4000
}
__base_MFlash64 = 0x00000000;
__base_Flash = 0x00000000;
__top_MFlash64 = 0x0000F000;
__top_Flash = 0x0000F000;
__base_RamLoc16 = 0x10000000;
__base_RAM = 0x10000000;
__top_RamLoc16 = 0x10004000;
__top_RAM = 0x10004000;
