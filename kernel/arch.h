#pragma once

/* Inicialización específica de arquitectura.
 * x86 → arch/x86/arch.c  configura GDT, IDT...
 * ARM → arch/arm/arch.c  configura MMU, excepciones...
 */
void arch_init(unsigned int boot_info);
