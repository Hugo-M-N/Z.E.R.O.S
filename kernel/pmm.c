/*
 * pmm.c — Gestor de memoria física
 *
 * Un bitmap donde cada bit representa una página de 4 KB:
 *   0 = libre   1 = usada
 *
 * El bitmap está en .bss (memoria estática del kernel), lo que
 * significa que él mismo ocupa páginas que hay que marcar como usadas.
 *
 * Soporta hasta 1 GB de RAM (262144 páginas → 32 KB de bitmap).
 * Para 4 GB habría que ampliar a 128 KB de bitmap.
 */

#include "pmm.h"
#include "console.h"

#define MAX_PAGES   (512 * 1024 * 8)    /* 4194304 páginas = 16 GB */
#define BITMAP_SIZE (MAX_PAGES / 32)    /* 131072 unsigned ints = 512 KB */

static unsigned int bitmap[BITMAP_SIZE];
static unsigned int total_pages = 0;
static unsigned int free_pages  = 0;

/* ── Operaciones sobre el bitmap ─────────────────────────*/
static void bit_set(unsigned int page) {
    bitmap[page / 32] |= (1u << (page % 32));
}

static void bit_clear(unsigned int page) {
    bitmap[page / 32] &= ~(1u << (page % 32));
}

static int bit_test(unsigned int page) {
    return (bitmap[page / 32] >> (page % 32)) & 1;
}

/* ── API pública ─────────────────────────────────────────*/

void pmm_init(unsigned int total_kb) {
    total_pages = (total_kb * 1024) / PAGE_SIZE;
    if (total_pages > MAX_PAGES) total_pages = MAX_PAGES;
    free_pages = 0;

    /* Empezar con todo marcado como usado — solo liberamos lo que
     * el mapa de memoria confirma explícitamente como disponible */
    for (unsigned int i = 0; i < BITMAP_SIZE; i++)
        bitmap[i] = 0xFFFFFFFF;
}

void pmm_mark_free(unsigned int base, unsigned int len) {
    unsigned int page_start = base / PAGE_SIZE;
    unsigned int page_end   = (base + len) / PAGE_SIZE;
    for (unsigned int p = page_start; p < page_end && p < total_pages; p++) {
        if (bit_test(p)) {
            bit_clear(p);
            free_pages++;
        }
    }
}

void pmm_mark_used(unsigned int base, unsigned int len) {
    unsigned int page_start = base / PAGE_SIZE;
    unsigned int page_end   = (base + len + PAGE_SIZE - 1) / PAGE_SIZE;
    for (unsigned int p = page_start; p < page_end && p < total_pages; p++) {
        if (!bit_test(p)) {
            bit_set(p);
            if (free_pages > 0) free_pages--;
        }
    }
}

void *pmm_alloc(void) {
    for (unsigned int i = 0; i < total_pages / 32; i++) {
        if (bitmap[i] == 0xFFFFFFFF) continue;  /* bloque lleno */
        for (int bit = 0; bit < 32; bit++) {
            unsigned int page = i * 32 + bit;
            if (!bit_test(page)) {
                bit_set(page);
                free_pages--;
                return (void *)(page * PAGE_SIZE);
            }
        }
    }
    return (void *)0;  /* sin memoria */
}

void pmm_free(void *addr) {
    unsigned int page = (unsigned int)addr / PAGE_SIZE;
    if (page < total_pages && bit_test(page)) {
        bit_clear(page);
        free_pages++;
    }
}

void pmm_print_stats(void) {
    unsigned int used = total_pages - free_pages;
    console_print("  RAM total: ");
    console_print_hex(total_pages * PAGE_SIZE);
    console_print(" (");
    console_print_hex(total_pages);
    console_print(" paginas)\n");
    console_print("  Libre:     ");
    console_print_hex(free_pages * PAGE_SIZE);
    console_print("  Usada: ");
    console_print_hex(used * PAGE_SIZE);
    console_print("\n");
}
