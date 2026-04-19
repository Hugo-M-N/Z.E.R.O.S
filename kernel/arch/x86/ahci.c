/*
 * ahci.c (x86) — Driver AHCI para discos SATA
 *
 * AHCI (Advanced Host Controller Interface) es el protocolo estándar para
 * acceder a controladores SATA modernos. A diferencia del ATA PIO clásico
 * (que usa puertos I/O), AHCI usa MMIO y DMA:
 *
 *   1. PCI: encontrar el controlador (clase 01h/06h/01h) y leer BAR5 (MMIO base)
 *   2. Mapear BAR5 en nuestra tabla de páginas (paging_map)
 *   3. Inicializar el puerto que tiene disco: configurar CLB, FB, arrancar engine
 *   4. Para cada operación: construir un Command FIS (H2D Register FIS)
 *      y un PRDT (descriptor de buffer DMA), luego esperar polling a CI=0
 *
 * Solo usamos el puerto 0 (primer disco) y solo 1 slot de comando a la vez.
 * Modo polling, sin IRQs de disco.
 */

#include "ahci.h"
#include "pci.h"
#include "paging.h"
#include "../../console.h"
#include "../../pmm.h"

/* ── Registros genéricos del HBA (en BAR5 + 0x000) ──────*/
#define HBA_GHC_AE   (1u << 31)  /* AHCI Enable */

/* ── Registros por puerto (en BAR5 + 0x100 + port*0x80) ─*/
#define PxCMD_ST     (1u << 0)   /* Start: command engine activo */
#define PxCMD_FRE    (1u << 4)   /* FIS Receive Enable */
#define PxCMD_FR     (1u << 14)  /* FIS Receive Running */
#define PxCMD_CR     (1u << 15)  /* Command List Running */
#define PxTFD_BSY    (1u << 7)
#define PxTFD_DRQ    (1u << 3)
#define PxSSTS_DET   0x0F        /* máscara Device Detection */
#define DET_PRESENT  0x03        /* dispositivo presente e interfaz establecida */
#define SATA_SIG_ATA 0x00000101  /* firma de disco ATA */

/* ── Tipos de FIS ────────────────────────────────────────*/
#define FIS_TYPE_H2D 0x27        /* Register FIS host-to-device */

/* ── Comandos ATA ────────────────────────────────────────*/
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35

/* ── Estructuras AHCI ────────────────────────────────────*/

typedef struct {
    volatile unsigned int cap, ghc, is, pi, vs;
    volatile unsigned int ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
} hba_mem_t;

typedef struct {
    volatile unsigned int clb, clbu, fb, fbu;
    volatile unsigned int is, ie, cmd, rsv0, tfd, sig;
    volatile unsigned int ssts, sctl, serr, sact, ci;
    volatile unsigned int sntf, fbs;
    volatile unsigned int rsv1[11];
    volatile unsigned int vendor[4];
} hba_port_t;

/* Command List Header (32 bytes, hay 32 por puerto) */
typedef struct __attribute__((packed)) {
    unsigned char  cfl;      /* bits [4:0]=CFL, [5]=A, [6]=W(write), [7]=P */
    unsigned char  flags;    /* bits [0]=R, [1]=B, [2]=C, [7:4]=PMP */
    unsigned short prdtl;    /* número de entradas PRDT */
    volatile unsigned int prdbc; /* bytes transferidos (escrito por HBA) */
    unsigned int   ctba;     /* base física de la Command Table */
    unsigned int   ctbau;    /* parte alta (siempre 0 en 32-bit) */
    unsigned int   rsv[4];
} hba_cmd_hdr_t;

/* PRDT Entry (16 bytes) — describe un buffer de datos para DMA */
typedef struct __attribute__((packed)) {
    unsigned int dba;    /* dirección física del buffer */
    unsigned int dbau;   /* parte alta (0 en 32-bit) */
    unsigned int rsv;
    unsigned int dbc;    /* byte count − 1 (bit 31 = interrupción al completar) */
} hba_prdt_t;

/* Command Table — contiene el FIS + PRDT */
typedef struct __attribute__((packed)) {
    unsigned char cfis[64];  /* Command FIS */
    unsigned char acmd[16];  /* ATAPI command (no usado aquí) */
    unsigned char rsv[48];
    hba_prdt_t    prdt[1];   /* un descriptor de buffer */
} hba_cmd_tbl_t;

/* Register FIS host-to-device (tipo 0x27) */
typedef struct __attribute__((packed)) {
    unsigned char fis_type;   /* 0x27 */
    unsigned char flags;      /* bit7=C (1=comando, 0=control), bits[3:0]=PMP */
    unsigned char command;    /* ATA command */
    unsigned char featurel;
    unsigned char lba0, lba1, lba2, device;
    unsigned char lba3, lba4, lba5, featureh;
    unsigned char countl, counth, icc, control;
    unsigned char rsv[4];
} fis_h2d_t;

/* ── Estado global del driver ────────────────────────────*/
static hba_port_t *g_port     = 0;
static unsigned int g_buf_phys = 0;   /* página para CLB + FIS + CTbl */

/* ── Helpers ─────────────────────────────────────────────*/

static void port_stop(hba_port_t *p) {
    p->cmd &= ~PxCMD_ST;
    for (int i = 0; i < 100000; i++)
        if (!(p->cmd & PxCMD_CR)) break;
    p->cmd &= ~PxCMD_FRE;
    for (int i = 0; i < 100000; i++)
        if (!(p->cmd & PxCMD_FR)) break;
}

static void port_start(hba_port_t *p) {
    for (int i = 0; i < 100000; i++)
        if (!(p->tfd & (PxTFD_BSY | PxTFD_DRQ))) break;
    p->cmd |= PxCMD_FRE;
    p->cmd |= PxCMD_ST;
}

static void memzero(void *dst, unsigned int n) {
    unsigned char *p = (unsigned char *)dst;
    for (unsigned int i = 0; i < n; i++) p[i] = 0;
}

/* ── Inicialización ──────────────────────────────────────*/

int ahci_init(void) {
    unsigned char bus, slot;
    unsigned int bar5 = pci_find_ahci(&bus, &slot);
    if (!bar5) {
        console_print("  [DISK] No se encontro controlador AHCI\n");
        return -1;
    }

    /* Mapear el espacio MMIO del HBA en nuestras tablas de páginas.
     * Los registros ocupan como máximo 0x100 + 32*0x80 = 0x1100 bytes.
     * Mapeamos 4 páginas (16 KB) para mayor seguridad. */
    for (int i = 0; i < 4; i++)
        paging_map(bar5 + (unsigned int)i * 4096,
                   bar5 + (unsigned int)i * 4096,
                   PAGE_PRESENT | PAGE_WRITE);

    hba_mem_t *hba = (hba_mem_t *)bar5;
    hba->ghc |= HBA_GHC_AE;   /* activar modo AHCI */

    /* Buscar el primer puerto con disco ATA (firma 0x00000101) */
    unsigned int pi = hba->pi;
    int port_num = -1;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        hba_port_t *p = (hba_port_t *)(bar5 + 0x100 + (unsigned int)i * 0x80);
        unsigned int det = p->ssts & PxSSTS_DET;
        unsigned int sig = p->sig;
        if (det == DET_PRESENT && sig == SATA_SIG_ATA && port_num < 0)
            port_num = i;
    }
    if (port_num < 0) {
        console_print("  [DISK] No se encontro disco SATA\n");
        return -1;
    }

    g_port = (hba_port_t *)(bar5 + 0x100 + (unsigned int)port_num * 0x80);

    /* ── Asignar una página para las estructuras de comando ─────
     *
     * Layout dentro de la página (4096 bytes):
     *   offset    0 : Command List  — 32 cabeceras × 32 bytes = 1024 bytes
     *                  alineación 1024 ✓ (la página está alineada a 4096)
     *   offset 1024 : FIS Receive Buffer — 256 bytes
     *                  alineación 256 ✓
     *   offset 1280 : Command Table — 128 + 16 PRDT ≈ 144 bytes
     *                  alineación 128 ✓ (1280 = 0x500, 0x500 % 128 = 0)
     */
    void *page = pmm_alloc();
    if (!page) {
        console_print("  [DISK] Sin memoria para AHCI\n");
        return -1;
    }
    g_buf_phys = (unsigned int)page;
    memzero(page, 4096);

    port_stop(g_port);

    g_port->clb  = g_buf_phys;          /* Command List Base */
    g_port->clbu = 0;
    g_port->fb   = g_buf_phys + 1024;   /* FIS Receive Base */
    g_port->fbu  = 0;
    g_port->is   = 0xFFFFFFFF;          /* limpiar interrupciones pendientes */
    g_port->ie   = 0;                   /* no usar IRQs (polling) */
    g_port->serr = 0xFFFFFFFF;

    port_start(g_port);

    console_print("  [OK] AHCI disco SATA (puerto ");
    console_print_hex((unsigned int)port_num);
    console_print(")\n");
    return 0;
}

/* ── Ejecutar un comando READ/WRITE DMA EXT ──────────────*/

static int do_rw(unsigned int lba, void *buf, int write) {
    if (!g_port) return -1;

    unsigned int hdr_phys = g_buf_phys + 0;       /* Command List Header */
    unsigned int tbl_phys = g_buf_phys + 1280;    /* Command Table */

    /* Cabecera del slot 0 */
    hba_cmd_hdr_t *hdr = (hba_cmd_hdr_t *)hdr_phys;
    memzero(hdr, sizeof(*hdr));
    /* CFL = tamaño del FIS en dwords (fis_h2d_t = 20 bytes = 5 dwords) */
    hdr->cfl   = 5 | (unsigned char)(write ? (1 << 6) : 0);
    hdr->prdtl = 1;
    hdr->ctba  = tbl_phys;
    hdr->ctbau = 0;

    /* Command Table */
    hba_cmd_tbl_t *tbl = (hba_cmd_tbl_t *)tbl_phys;
    memzero(tbl, sizeof(*tbl));

    /* PRDT: un bloque de 512 bytes */
    tbl->prdt[0].dba  = (unsigned int)buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = 512 - 1;   /* byte count − 1 */

    /* Command FIS (Register H2D) */
    fis_h2d_t *fis = (fis_h2d_t *)tbl->cfis;
    fis->fis_type = FIS_TYPE_H2D;
    fis->flags    = (1 << 7);      /* C=1: acceso al registro de comando */
    fis->command  = write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT;
    fis->device   = (1 << 6);      /* LBA mode */
    fis->lba0 = (unsigned char)( lba        & 0xFF);
    fis->lba1 = (unsigned char)((lba >>  8) & 0xFF);
    fis->lba2 = (unsigned char)((lba >> 16) & 0xFF);
    fis->lba3 = (unsigned char)((lba >> 24) & 0xFF);
    fis->lba4 = 0;
    fis->lba5 = 0;
    fis->countl = 1;
    fis->counth = 0;

    /* Limpiar estado e issuar el slot 0 */
    g_port->is = 0xFFFFFFFF;
    g_port->ci = 1;

    /* Esperar a que CI[0] = 0 (comando completado).
     * Desactivamos interrupciones durante el polling para que el scheduler
     * no nos quite la CPU: si hay un context-switch en medio, el contador
     * se agota antes de que el disco termine y el comando falla. */
    __asm__ volatile("cli");
    int done = 0;
    for (int i = 0; i < 2000000; i++) {
        if (!(g_port->ci & 1)) { done = 1; break; }
    }

    if (!done) {
        __asm__ volatile("sti");
        return -1;
    }
    if (g_port->is & (1u << 30)) {
        __asm__ volatile("sti");
        return -1;
    }

    if (!write) {
        unsigned char *p = (unsigned char *)buf;
        for (unsigned int i = 0; i < 512; i += 64)
            __asm__ volatile("clflush (%0)" : : "r"(p + i) : "memory");
        __asm__ volatile("mfence" : : : "memory");
    }

    __asm__ volatile("sti");
    return 0;
}

int ahci_read_sector(unsigned int lba, void *buf) {
    return do_rw(lba, buf, 0);
}

int ahci_write_sector(unsigned int lba, const void *buf) {
    return do_rw(lba, (void *)buf, 1);
}
