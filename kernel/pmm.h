#pragma once

/* Gestor de memoria física (Physical Memory Manager)
 * Unidad de asignación: página de 4 KB.
 * Implementación: bitmap — 1 bit por página (0=libre, 1=usada). */

#define PAGE_SIZE 4096

void  pmm_init(unsigned int total_kb);
void  pmm_mark_free(unsigned int base, unsigned int len);
void  pmm_mark_used(unsigned int base, unsigned int len);
void *pmm_alloc(void);
void  pmm_free(void *page);
void  pmm_print_stats(void);
