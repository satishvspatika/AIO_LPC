/* Long-name character conversion for ASCII names only (v6.9.65, in place of ccsbcs.c).
 *
 * Every name this firmware opens or makes is ASCII (fw_ksn_trg.hex needs long names, nothing needs
 * accents). ccsbcs.c carried code page 1252 and upper-case tables for Greek, Cyrillic and full-width
 * letters: 1.2 KB of flash that no name on the card uses.
 *
 * What changes for a name with a character above 0x7F, left on the card by a PC:
 *  - f_readdir gives its 8.3 name instead of its long name (ff.c drops a long name it cannot convert).
 *  - It cannot be opened by its long name. The firmware never asks for such a name.
 * ASCII names behave exactly as before. */
#include "../ff.h"

#if _USE_LFN
WCHAR ff_convert(WCHAR src, UINT dir)
{
    (void)dir;
    return src < 0x80 ? src : 0;    /* 0: cannot convert */
}

WCHAR ff_wtoupper(WCHAR chr)
{
    return (chr >= 'a' && chr <= 'z') ? (WCHAR)(chr - 0x20) : chr;
}
#endif
