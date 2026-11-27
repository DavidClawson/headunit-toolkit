/* uart.h: polled 16550 UART on the F133 (UART n at 0x02500000 + 0x400 n). */
#pragma once
#include <stdint.h>
void uart_init(unsigned n, unsigned baud);  /* UART1 also gets PG12 TX / PG13 RX (function 7) */
int  uart_getc(unsigned n);                 /* next byte, or -1 if none */
void uart_putc(unsigned n, uint8_t c);
