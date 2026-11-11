/* Migration image: board access (time, LCD, pins, the CTRL-link UART). */
#ifndef MIG_HAL_H
#define MIG_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void hal_init(void);
uint32_t hal_ms(void);
void hal_delay_ms(uint32_t ms);

/* LCD, 16 x 2. Lines are padded or cut to 16 characters. */
void lcd_init(void);
void lcd_show(const char *line1, const char *line2);

/* The two jumper lines to the LPC845: RST_845 (P0.5, JP1) and -PGM_845 (P1.0, JP2) */
typedef enum { LINE_RST845, LINE_PGM845 } mig_line_t;
void line_drive(mig_line_t line, bool high);
void line_release(mig_line_t line);         /* Input with pull-up, as the old and new firmware leave it */
bool line_jumper_fitted(mig_line_t line);   /* Pull-down for a moment: the 845 board's 4K7 pull-up wins only through a fitted jumper */

/* UART3 (P4.28 TXD3, P4.29 RXD3): the CTRL link, and the LPC845 ROM loader's UART */
void uart3_open(uint32_t baud);
void uart3_flush_rx(void);
void uart3_write(const void *data, size_t len);
void uart3_puts(const char *s);
int uart3_getc(uint32_t timeout_ms);        /* -1 on timeout */

/* Small text helpers (no printf in this image) */
char *fmt_hex(char *out, uint32_t value, int digits);
char *fmt_dec(char *out, uint32_t value);

#endif
