/*
 * scheduler.c — Gestión de procesos y scheduler round-robin
 *
 * Cada proceso tiene un PCB (Process Control Block) con su estado
 * y su stack pointer guardado. El context switch ocurre en irq_common:
 *
 *   1. Timer interrumpe al proceso actual
 *   2. irq_common guarda todos los registros en el stack del proceso
 *   3. irq_handler llama a scheduler_tick(esp_actual)
 *   4. scheduler_tick guarda esp en el PCB actual, elige el siguiente
 *   5. Devuelve el esp del siguiente proceso
 *   6. irq_common carga ese esp y hace iret → corre el siguiente proceso
 *
 * El stack inicial de cada proceso nuevo se construye con el mismo
 * layout que deja irq_common, para que el primer iret salte a su
 * función de entrada con interrupciones activadas.
 *
 * NOTA: las funciones de proceso no deben retornar (usar while(1)).
 */

#include "scheduler.h"
#include "pmm.h"
#include "console.h"

#define PROC_MAX    16
#define STACK_SIZE  4096

typedef enum { PROC_FREE = 0, PROC_READY, PROC_RUNNING } proc_state_t;

struct pcb {
    unsigned int   pid;
    proc_state_t   state;
    unsigned int   esp;         /* stack pointer guardado */
    void          *stack_page;  /* página del stack (para liberar) */
};

static struct pcb procs[PROC_MAX];
static int current = 0;
static int count   = 0;

void scheduler_init(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        procs[i].pid        = 0;
        procs[i].state      = PROC_FREE;
        procs[i].esp        = 0;
        procs[i].stack_page = 0;
    }
    /* Proceso 0 = contexto actual (kmain sobre el stack de arranque) */
    procs[0].pid   = 0;
    procs[0].state = PROC_RUNNING;
    current        = 0;
    count          = 1;
}

int process_create(void (*entry)(void)) {
    int slot = -1;
    for (int i = 1; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_FREE) { slot = i; break; }
    }
    if (slot < 0) return -1;

    void *page = pmm_alloc();
    if (!page) return -1;

    /* ── Construir el stack inicial ──────────────────────
     *
     * irq_common restaura en este orden (dirección baja → alta):
     *   pop gs, pop fs, pop es, pop ds   ← gs queda en la dirección más baja
     *   popa (edi, esi, ebp, skip, ebx, edx, ecx, eax)
     *   add esp, 8                        ← salta int_no + err_code
     *   iret (eip, cs, eflags)            ← en la dirección más alta
     *
     * Escribimos de alta a baja con --sp.  Lo último que escribimos (gs)
     * queda en la dirección más baja y es donde apunta el esp guardado.
     */
    unsigned int *sp = (unsigned int *)((unsigned char *)page + STACK_SIZE);

    /* iret frame — al tope del stack (dirección más alta) */
    *--sp = 0x00000202;             /* eflags: IF=1 */
    *--sp = 0x08;                   /* cs */
    *--sp = (unsigned int)entry;    /* eip */

    /* Saltados por "add esp, 8" */
    *--sp = 0;                      /* err_code */
    *--sp = 0;                      /* int_no */

    /* Registros generales — popa lee edi primero (dirección baja) y eax último.
     * Escribimos eax primero (dirección alta) y edi último (más baja). */
    *--sp = 0;  /* eax */
    *--sp = 0;  /* ecx */
    *--sp = 0;  /* edx */
    *--sp = 0;  /* ebx */
    *--sp = 0;  /* esp dummy (popa lo ignora) */
    *--sp = 0;  /* ebp */
    *--sp = 0;  /* esi */
    *--sp = 0;  /* edi */

    /* Registros de segmento — pop gs/fs/es/ds lee gs primero (dirección baja).
     * Escribimos ds primero (dirección alta) y gs último (más baja). */
    *--sp = 0x10;                   /* ds */
    *--sp = 0x10;                   /* es */
    *--sp = 0x10;                   /* fs */
    *--sp = 0x10;                   /* gs ← esp guardado apunta aquí */

    procs[slot].pid        = (unsigned int)slot;
    procs[slot].state      = PROC_READY;
    procs[slot].esp        = (unsigned int)sp;
    procs[slot].stack_page = page;
    count++;
    console_print("  [DBG] proceso creado entry=");
    console_print_hex((unsigned int)entry);
    console_print(" stack_esp=");
    console_print_hex((unsigned int)sp);
    console_print("\n");
    return slot;
}

/* Llamado desde irq_handler con el esp actual (= puntero al frame guardado).
 * Devuelve el esp del siguiente proceso, o 0 si no hay cambio. */
unsigned int scheduler_tick(unsigned int current_esp) {
    if (count <= 1) return 0;

    procs[current].esp   = current_esp;
    procs[current].state = PROC_READY;

    /* Round-robin: buscar el siguiente proceso READY */
    int next = (current + 1) % PROC_MAX;
    for (int i = 0; i < PROC_MAX; i++, next = (next + 1) % PROC_MAX)
        if (procs[next].state == PROC_READY) break;

    if (next == current) { procs[current].state = PROC_RUNNING; return 0; }

    current              = next;
    procs[next].state    = PROC_RUNNING;
    return procs[next].esp;
}
