/*
 * pic.c (x86) — Programmable Interrupt Controller 8259
 *
 * El PC tiene dos PICs en cascada (maestro y esclavo) que convierten
 * señales hardware (teclado, timer, disco...) en interrupciones CPU.
 *
 * Por defecto los IRQs 0-15 se mapean a vectores 8-15 y 112-119,
 * que colisionan con las excepciones de la CPU (0-31). Hay que
 * remapearlos a vectores 32-47 antes de activar interrupciones.
 *
 * En ARM este archivo no existe — el RPi usa el GIC (Generic
 * Interrupt Controller), que es un hardware completamente distinto.
 */

#include "pic.h"

/* Puertos I/O del PIC */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI   0x20   /* comando End Of Interrupt */

/* ICW = Initialization Command Word */
#define ICW1_INIT 0x11   /* inicializar en modo cascada, ICW4 presente */
#define ICW4_8086 0x01   /* modo 8086 */

static void outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static unsigned char inb(unsigned short port) {
    unsigned char val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void io_wait(void) { inb(0x80); }

void pic_init(void) {
    /* Guardar máscaras actuales */
    unsigned char m1 = inb(PIC1_DATA);
    unsigned char m2 = inb(PIC2_DATA);

    /* ICW1: iniciar secuencia de inicialización */
    outb(PIC1_CMD,  ICW1_INIT); io_wait();
    outb(PIC2_CMD,  ICW1_INIT); io_wait();

    /* ICW2: remapear vectores — IRQ 0-7 → 32-39, IRQ 8-15 → 40-47 */
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();

    /* ICW3: cascada (IRQ2 del maestro conectado al esclavo) */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    /* ICW4: modo 8086 */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Restaurar máscaras */
    outb(PIC1_DATA, m1);
    outb(PIC2_DATA, m2);
}

void pic_eoi(unsigned int irq) {
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_unmask(unsigned int irq) {
    unsigned short port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    unsigned char bit   = (unsigned char)(1u << (irq & 7));
    outb(port, inb(port) & ~bit);
}

void pic_mask(unsigned int irq) {
    unsigned short port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    unsigned char bit   = (unsigned char)(1u << (irq & 7));
    outb(port, inb(port) | bit);
}
