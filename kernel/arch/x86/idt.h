#pragma once

/* Registros guardados al entrar en una interrupción.
 * El orden refleja exactamente el layout del stack en isr_common. */
struct registers {
    unsigned int gs, fs, es, ds;
    unsigned int edi, esi, ebp, esp, ebx, edx, ecx, eax; /* pusha */
    unsigned int int_no, err_code;
    unsigned int eip, cs, eflags;                         /* CPU */
};

/* Tipo de función manejadora de IRQ */
typedef void (*irq_handler_t)(struct registers *);
typedef unsigned int (*reschedule_fn_t)(unsigned int esp);

void idt_init(void);
void irq_install_handler(int irq, irq_handler_t fn);
void idt_set_scheduler(reschedule_fn_t fn);
