#pragma once

/* Interfaz de consola independiente de la arquitectura.
 * x86  → arch/x86/console.c  usa el buffer VGA (0xB8000)
 * ARM  → arch/arm/console.c  usará el UART (PL011 en RPi)
 */
void console_clear(void);
void console_print    (const char *s);
void console_print_hex(unsigned int n);
void console_print_uint(unsigned int n);
void console_print_buf (const char *buf, unsigned int len);
