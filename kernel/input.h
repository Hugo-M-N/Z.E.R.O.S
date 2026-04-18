#pragma once

/* Interfaz arch-independiente de entrada de teclado.
 * x86: keyboard.c (PS/2, IRQ1)
 * ARM: implementar con UART RX o GPIO */

void input_init   (void);
int  input_getchar(void);   /* -1 si no hay nada en el buffer */
char input_readchar(void);  /* bloquea hasta recibir un carácter */
