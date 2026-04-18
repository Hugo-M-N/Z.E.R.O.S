/*
 * arch.c (x86) — Inicialización de arquitectura
 *
 * Recibe el puntero multiboot_info del bootloader y lo usa para
 * inicializar el PMM con el mapa de memoria real de la máquina.
 * En ARM recibirá el device tree en su lugar.
 */

#include "../../arch.h"
#include "../../pmm.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "paging.h"
#include "timer.h"
#include "multiboot.h"
#include "../../console.h"
#include "../../scheduler.h"
#include "../../disk.h"
#include "../../input.h"

/* Símbolo del linker — dirección justo después del kernel en memoria */
extern unsigned int kernel_end;

void arch_init(unsigned int boot_info) {
    gdt_init();
    idt_init();
    pic_init();

    /* ── Inicializar PMM con el mapa de memoria real ─────
     *
     * multiboot_info.mem_upper es la RAM por encima de 1 MB en KB.
     * El total real es mem_upper + 1 MB (los primeros 1024 KB).
     */
    struct multiboot_info *mb = (struct multiboot_info *)boot_info;
    unsigned int total_kb = mb->mem_upper + 1024;
    pmm_init(total_kb);

    /* Marcar como libres las regiones que el bootloader reporta */
    if (mb->flags & MULTIBOOT_FLAG_MMAP) {
        unsigned int addr = mb->mmap_addr;
        unsigned int end  = mb->mmap_addr + mb->mmap_length;
        while (addr < end) {
            struct mmap_entry *e = (struct mmap_entry *)addr;
            /* Solo regiones disponibles y dentro de los 32 bits */
            if (e->type == 1 && e->base_high == 0)
                pmm_mark_free(e->base_low, e->len_low);
            addr += e->size + 4;
        }
    } else {
        /* Sin mapa detallado: marcar solo mem_upper como libre */
        pmm_mark_free(0x100000, mb->mem_upper * 1024);
    }

    /* Marcar el kernel (0 → kernel_end) como usado */
    pmm_mark_used(0, (unsigned int)&kernel_end);

    console_print("  [OK] PMM inicializado\n");
    pmm_print_stats();

    paging_init();
    disk_init();
    scheduler_init();
    timer_init(100);
    idt_set_scheduler(scheduler_tick);

    __asm__ volatile("sti");
    input_init();
}
