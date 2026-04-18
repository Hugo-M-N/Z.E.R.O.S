/*
 * console.c (ARM) — Driver UART para Raspberry Pi
 *
 * El RPi expone un UART PL011 en la dirección base 0x3F201000 (RPi 2/3)
 * o 0xFE201000 (RPi 4). Escribir en el registro DR (offset 0x00) envía
 * un byte por el puerto serie.
 *
 * TODO: implementar cuando portemos a RPi.
 *   - console_clear(): no aplica en UART, podría enviar secuencia ANSI
 *   - console_print(): escribir byte a byte en UART_DR
 *   - console_print_hex(): igual que x86
 */

#include "../../console.h"

/* RPi 2/3 — cambiar a 0xFE201000 para RPi 4 */
#define UART_BASE 0x3F201000
#define UART_DR   (*(volatile unsigned int *)(UART_BASE + 0x00))
#define UART_FR   (*(volatile unsigned int *)(UART_BASE + 0x18))
#define UART_FR_TXFF (1 << 5)   /* TX FIFO llena */

static void uart_putchar(char c) {
    while (UART_FR & UART_FR_TXFF) {}  /* esperar a que haya hueco */
    UART_DR = (unsigned int)(unsigned char)c;
}

void console_clear(void) {
    /* UART no tiene "limpiar pantalla" — enviar saltos de línea */
    for (int i = 0; i < 40; i++) uart_putchar('\n');
}

void console_print(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putchar('\r');
        uart_putchar(*s++);
    }
}

void console_print_hex(unsigned int n) {
    console_print("0x");
    for (int i = 28; i >= 0; i -= 4) {
        unsigned char d = (n >> i) & 0xF;
        uart_putchar(d < 10 ? '0' + d : 'A' + d - 10);
    }
}
