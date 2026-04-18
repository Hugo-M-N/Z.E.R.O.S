#include "console.h"
#include "arch.h"
#include "heap.h"
#include "kshell.h"
#include "scheduler.h"

#define MULTIBOOT_MAGIC 0x2BADB002

void kmain(unsigned int magic, unsigned int info) {
    (void)info;

    console_clear();
    console_print("  Z.E.R.O.S Kernel v0.1\n\n");

    if (magic != MULTIBOOT_MAGIC) {
        console_print("  [!!] Magic incorrecto\n");
        for (;;) __asm__ volatile("hlt");
    }

    arch_init(info);
    heap_init();

    process_create(kshell_run);

    /* Bucle idle del kernel — el scheduler alterna con kshell */
    for (;;) __asm__ volatile("hlt");
}
