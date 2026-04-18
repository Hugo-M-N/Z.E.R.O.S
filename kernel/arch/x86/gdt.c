/*
 * gdt.c (x86) — Global Descriptor Table
 *
 * En x86 protegido cada acceso a memoria pasa por un segmento.
 * La GDT define qué segmentos existen, su base, límite y permisos.
 *
 * ARM no tiene GDT — usa una MMU con page tables directamente.
 * Por eso este archivo vive en arch/x86/ y no en el kernel común.
 *
 * Entradas:
 *   índice 0 — null     (obligatorio, la CPU lo requiere)
 *   índice 1 — código   selector 0x08, ring 0, ejecutable
 *   índice 2 — datos    selector 0x10, ring 0, lectura/escritura
 *
 * En el futuro añadiremos índices 3 y 4 para código y datos de
 * procesos de usuario (ring 3).
 */

#include "gdt.h"
#include "../../console.h"

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_mid;
    unsigned char  access;
    unsigned char  granularity;
    unsigned char  base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr   gdt_p;

extern void gdt_flush(unsigned int);

static void gdt_set(int i, unsigned int base, unsigned int limit,
                    unsigned char access, unsigned char gran) {
    gdt[i].base_low    =  base        & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   =  limit       & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

void gdt_init(void) {
    gdt_p.limit = sizeof(gdt) - 1;
    gdt_p.base  = (unsigned int)&gdt;

    gdt_set(0, 0, 0,       0x00, 0x00); /* null                */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF); /* kernel code — 0x08 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF); /* kernel data — 0x10 */

    gdt_flush((unsigned int)&gdt_p);
    console_print("  [OK] GDT (null / code 0x08 / data 0x10)\n");
}
