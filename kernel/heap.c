/*
 * heap.c — Allocador de heap del kernel (kmalloc / kfree)
 *
 * Implementación: lista enlazada de bloques (first-fit).
 * Cada bloque tiene un header seguido de los datos del usuario:
 *
 *   [ block_hdr | ← size bytes → ] [ block_hdr | ← size bytes → ] ...
 *
 * Cuando no hay bloque libre suficiente se pide una página al PMM.
 * Con paginación identity-mapped, la dirección física == virtual.
 *
 * Operaciones:
 *   kmalloc(n) — primer bloque libre ≥ n; lo parte si sobra espacio
 *   kfree(p)   — marca libre + fusiona bloques adyacentes (coalescing)
 */

#include "heap.h"
#include "pmm.h"
#include "console.h"

/* Alinear a 8 bytes para que cualquier tipo quepa correctamente */
#define ALIGN8(x)   (((x) + 7u) & ~7u)
#define HDR_SIZE    ALIGN8(sizeof(struct block_hdr))
#define MIN_SPLIT   (HDR_SIZE + 8u)   /* mínimo útil para partir un bloque */

struct block_hdr {
    unsigned int      size;   /* bytes de datos, sin contar el header */
    int               free;   /* 1 = libre, 0 = ocupado */
    struct block_hdr *next;
};

static struct block_hdr *heap_head = 0;

/* ── Pedir una página al PMM y añadirla al heap ──────────*/
static struct block_hdr *heap_grow(void) {
    void *page = pmm_alloc();
    if (!page) return 0;

    struct block_hdr *blk = (struct block_hdr *)page;
    blk->size = PAGE_SIZE - HDR_SIZE;
    blk->free = 1;
    blk->next = 0;

    if (!heap_head) {
        heap_head = blk;
        return blk;
    }

    /* Añadir al final — fusionar si el último bloque también es libre */
    struct block_hdr *cur = heap_head;
    while (cur->next) cur = cur->next;

    if (cur->free) {
        /* Solo fusionar si las páginas son físicamente contiguas */
        unsigned char *cur_end = (unsigned char *)cur + HDR_SIZE + cur->size;
        if (cur_end == (unsigned char *)blk) {
            cur->size += HDR_SIZE + blk->size;
            return cur;
        }
    }
    cur->next = blk;
    return blk;
}

void heap_init(void) {
    heap_grow();
    console_print("  [OK] Heap listo\n");
}

/* ── kmalloc ─────────────────────────────────────────────*/
void *kmalloc(unsigned int size) {
    if (!size) return 0;
    size = ALIGN8(size);

    struct block_hdr *cur = heap_head;
    while (1) {
        if (!cur) {
            cur = heap_grow();
            if (!cur) return 0;   /* sin memoria */
        }
        if (cur->free && cur->size >= size) {
            /* Partir el bloque si queda espacio suficiente */
            if (cur->size >= size + MIN_SPLIT) {
                struct block_hdr *rest =
                    (struct block_hdr *)((unsigned char *)cur + HDR_SIZE + size);
                rest->size = cur->size - size - HDR_SIZE;
                rest->free = 1;
                rest->next = cur->next;
                cur->size  = size;
                cur->next  = rest;
            }
            cur->free = 0;
            return (unsigned char *)cur + HDR_SIZE;
        }
        cur = cur->next;
    }
}

/* ── kfree ───────────────────────────────────────────────*/
void kfree(void *ptr) {
    if (!ptr) return;

    struct block_hdr *blk =
        (struct block_hdr *)((unsigned char *)ptr - HDR_SIZE);
    blk->free = 1;

    /* Fusionar bloques libres consecutivos */
    struct block_hdr *cur = heap_head;
    while (cur) {
        if (cur->free && cur->next && cur->next->free) {
            cur->size += HDR_SIZE + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}
