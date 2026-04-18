/*
 * timer.c (x86) — Programmable Interval Timer (PIT 8253)
 *
 * El PIT tiene una frecuencia base de 1.193182 MHz.
 * Para obtener N interrupciones por segundo: divisor = 1193182 / N.
 *
 * Conectado a IRQ0. Cada interrupción incrementa el contador de ticks.
 * El scheduler se engancha en irq_handler (no aquí) para separar
 * la lógica de hardware de la lógica de scheduling.
 */

#include "timer.h"
#include "idt.h"

#define PIT_BASE_HZ  1193182
#define PIT_CMD      0x43
#define PIT_CH0      0x40

static unsigned int ticks = 0;

static void outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void timer_handler(struct registers *r) {
    (void)r;
    ticks++;
}

void timer_init(unsigned int hz) {
    unsigned int divisor = PIT_BASE_HZ / hz;
    outb(PIT_CMD, 0x36);                      /* canal 0, modo 3, lo+hi byte */
    outb(PIT_CH0, (unsigned char)(divisor & 0xFF));
    outb(PIT_CH0, (unsigned char)(divisor >> 8));
    irq_install_handler(0, timer_handler);
}

unsigned int timer_ticks(void) { return ticks; }
