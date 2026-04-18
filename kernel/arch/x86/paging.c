/*
 * paging.c (x86) — Paginación de memoria
 *
 * x86 usa una estructura de dos niveles de 4 KB cada una:
 *
 *   Page Directory (1024 entradas × 4 bytes)
 *     └─ cada entrada apunta a una Page Table (1024 entradas × 4 bytes)
 *          └─ cada entrada apunta a una página física de 4 KB
 *
 * Una Page Table cubre 1024 × 4 KB = 4 MB.
 * El Page Directory cubre 1024 × 4 MB = 4 GB.
 *
 * Dirección virtual:
 *   bits 31-22 → índice en Page Directory  (qué Page Table)
 *   bits 21-12 → índice en Page Table      (qué página)
 *   bits 11-0  → offset dentro de la página
 *
 * Para el arranque hacemos identity mapping de los primeros 8 MB:
 * virtual == físico. Así el código del kernel sigue funcionando
 * igual después de activar CR0.PG.
 *
 * En ARM la MMU usa page tables de un nivel o dos niveles con un
 * formato completamente distinto — todo esto queda en arch/x86/.
 */

#include "paging.h"
#include "../../pmm.h"
#include "../../console.h"

/* Page Directory principal del kernel */
static unsigned int page_dir[1024] __attribute__((aligned(4096)));

/* Page Tables estáticas para los primeros 8 MB del kernel.
 * Son estáticas porque las necesitamos antes de que el PMM
 * esté completamente operativo con paginación activa. */
static unsigned int pt_kernel[2][1024] __attribute__((aligned(4096)));

static void cr3_load(unsigned int *dir) {
    __asm__ volatile("mov %0, %%cr3" : : "r"((unsigned int)dir) : "memory");
}

static void paging_enable(void) {
    unsigned int cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 31);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/* ── Mapear una página virtual a una física ──────────────
 *
 * Si la Page Table para esa dirección no existe todavía,
 * se asigna una nueva página física con pmm_alloc().
 */
void paging_map(unsigned int virt, unsigned int phys, unsigned int flags) {
    unsigned int dir_idx = virt >> 22;
    unsigned int tbl_idx = (virt >> 12) & 0x3FF;

    unsigned int *pt;
    if (!(page_dir[dir_idx] & PAGE_PRESENT)) {
        pt = (unsigned int *)pmm_alloc();
        if (!pt) return;
        for (int i = 0; i < 1024; i++) pt[i] = 0;
        page_dir[dir_idx] = (unsigned int)pt | PAGE_PRESENT | PAGE_WRITE;
    } else {
        pt = (unsigned int *)(page_dir[dir_idx] & ~0xFFF);
    }

    pt[tbl_idx] = (phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
}

void paging_init(void) {
    for (int i = 0; i < 1024; i++)
        page_dir[i] = 0;

    /* Identity map primeros 8 MB: virt == phys para el kernel */
    for (int t = 0; t < 2; t++) {
        for (int p = 0; p < 1024; p++) {
            unsigned int phys = ((unsigned int)t * 1024 + (unsigned int)p) * 0x1000;
            pt_kernel[t][p] = phys | PAGE_PRESENT | PAGE_WRITE;
        }
        page_dir[t] = (unsigned int)pt_kernel[t] | PAGE_PRESENT | PAGE_WRITE;
    }

    cr3_load(page_dir);
    paging_enable();
    console_print("  [OK] Paginacion (8 MB identity mapped)\n");
}
