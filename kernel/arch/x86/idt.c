/*
 * idt.c (x86) — Interrupt Descriptor Table
 *
 * La IDT tiene 256 entradas, una por vector de interrupción:
 *   0-31  → excepciones de la CPU (divide by zero, page fault...)
 *   32-47 → IRQs hardware remapeadas (timer, teclado...)
 *   48+   → disponibles para syscalls y futuras extensiones
 *
 * Cada entrada apunta al stub de assembly correspondiente en isr.s,
 * que guarda los registros y llama a isr_handler / irq_handler en C.
 */

#include "idt.h"
#include "pic.h"
#include "../../console.h"

/* ── Estructura de una entrada IDT (8 bytes) ─────────────*/
struct idt_entry {
    unsigned short base_low;
    unsigned short selector;   /* siempre 0x08 — kernel code */
    unsigned char  zero;
    unsigned char  flags;      /* 0x8E = presente, ring 0, int gate 32-bit */
    unsigned short base_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idt_p;

static irq_handler_t  irq_handlers[16] = {0};
static reschedule_fn_t reschedule_fn    = 0;

/* lidt — en gdt_flush.s reutilizamos el patrón, aquí inline */
static void idt_flush(unsigned int ptr) {
    __asm__ volatile("lidt (%0)" : : "r"(ptr));
}

static void idt_set_gate(unsigned char n, unsigned int base) {
    idt[n].base_low  = base & 0xFFFF;
    idt[n].base_high = (base >> 16) & 0xFFFF;
    idt[n].selector  = 0x08;
    idt[n].zero      = 0;
    idt[n].flags     = 0x8E;
}

/* ── Nombres de las excepciones CPU ─────────────────────*/
static const char *exc_names[] = {
    "Division por cero",       "Debug",
    "NMI",                     "Breakpoint",
    "Overflow",                "Bound range exceeded",
    "Opcode invalido",         "FPU no disponible",
    "Double fault",            "Coprocessor overrun",
    "TSS invalida",            "Segmento no presente",
    "Stack-segment fault",     "General Protection Fault",
    "Page Fault",              "Reservada",
    "Error FPU x87",           "Alignment check",
    "Machine check",           "SIMD FP exception",
};

/* ── Declaraciones de los stubs de isr.s ────────────────*/
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

void idt_init(void) {
    idt_p.limit = sizeof(idt) - 1;
    idt_p.base  = (unsigned int)&idt;

    /* Excepciones */
    idt_set_gate( 0, (unsigned int)isr0);
    idt_set_gate( 1, (unsigned int)isr1);
    idt_set_gate( 2, (unsigned int)isr2);
    idt_set_gate( 3, (unsigned int)isr3);
    idt_set_gate( 4, (unsigned int)isr4);
    idt_set_gate( 5, (unsigned int)isr5);
    idt_set_gate( 6, (unsigned int)isr6);
    idt_set_gate( 7, (unsigned int)isr7);
    idt_set_gate( 8, (unsigned int)isr8);
    idt_set_gate( 9, (unsigned int)isr9);
    idt_set_gate(10, (unsigned int)isr10);
    idt_set_gate(11, (unsigned int)isr11);
    idt_set_gate(12, (unsigned int)isr12);
    idt_set_gate(13, (unsigned int)isr13);
    idt_set_gate(14, (unsigned int)isr14);
    idt_set_gate(15, (unsigned int)isr15);
    idt_set_gate(16, (unsigned int)isr16);
    idt_set_gate(17, (unsigned int)isr17);
    idt_set_gate(18, (unsigned int)isr18);
    idt_set_gate(19, (unsigned int)isr19);
    idt_set_gate(20, (unsigned int)isr20);
    idt_set_gate(21, (unsigned int)isr21);
    idt_set_gate(22, (unsigned int)isr22);
    idt_set_gate(23, (unsigned int)isr23);
    idt_set_gate(24, (unsigned int)isr24);
    idt_set_gate(25, (unsigned int)isr25);
    idt_set_gate(26, (unsigned int)isr26);
    idt_set_gate(27, (unsigned int)isr27);
    idt_set_gate(28, (unsigned int)isr28);
    idt_set_gate(29, (unsigned int)isr29);
    idt_set_gate(30, (unsigned int)isr30);
    idt_set_gate(31, (unsigned int)isr31);

    /* IRQs hardware (vectores 32-47) */
    idt_set_gate(32, (unsigned int)irq0);
    idt_set_gate(33, (unsigned int)irq1);
    idt_set_gate(34, (unsigned int)irq2);
    idt_set_gate(35, (unsigned int)irq3);
    idt_set_gate(36, (unsigned int)irq4);
    idt_set_gate(37, (unsigned int)irq5);
    idt_set_gate(38, (unsigned int)irq6);
    idt_set_gate(39, (unsigned int)irq7);
    idt_set_gate(40, (unsigned int)irq8);
    idt_set_gate(41, (unsigned int)irq9);
    idt_set_gate(42, (unsigned int)irq10);
    idt_set_gate(43, (unsigned int)irq11);
    idt_set_gate(44, (unsigned int)irq12);
    idt_set_gate(45, (unsigned int)irq13);
    idt_set_gate(46, (unsigned int)irq14);
    idt_set_gate(47, (unsigned int)irq15);

    idt_flush((unsigned int)&idt_p);
    console_print("  [OK] IDT (32 excepciones + 16 IRQs)\n");
}

/* ── Manejador de excepciones CPU ───────────────────────*/
void isr_handler(struct registers *r) {
    console_print("\n  *** EXCEPCION ");
    if (r->int_no < 20)
        console_print(exc_names[r->int_no]);
    else
        console_print_hex(r->int_no);
    console_print(" ***\n  err=");
    console_print_hex(r->err_code);
    console_print("  eip=");
    console_print_hex(r->eip);
    console_print("\n");

    for (;;) __asm__ volatile("hlt");
}

/* ── Manejador de IRQs hardware ─────────────────────────*/
void irq_install_handler(int irq, irq_handler_t fn) {
    irq_handlers[irq] = fn;
}

void idt_set_scheduler(reschedule_fn_t fn) {
    reschedule_fn = fn;
}

/* Devuelve el nuevo esp si el scheduler quiere cambiar de proceso, 0 si no */
unsigned int irq_handler(struct registers *r) {
    unsigned int irq = r->int_no - 32;
    if (irq_handlers[irq])
        irq_handlers[irq](r);
    pic_eoi(irq);
    if (reschedule_fn)
        return reschedule_fn((unsigned int)r);
    return 0;
}
