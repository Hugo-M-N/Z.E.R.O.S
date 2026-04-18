/* Implementación de input.h para x86 — delega en keyboard.c */
#include "keyboard.h"
#include "../../input.h"

void input_init(void) { keyboard_init(); }

int input_getchar(void) { return keyboard_getchar(); }

char input_readchar(void) {
    int c;
    /* Esperar con hlt para no quemar CPU.
     * Las interrupciones están activas (sti en arch_init),
     * así que el próximo IRQ1 despertará la CPU del hlt. */
    while ((c = keyboard_getchar()) == -1)
        __asm__ volatile("hlt");
    return (char)c;
}
