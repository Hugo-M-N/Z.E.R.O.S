/*
 * zeros_populate.c — Copia archivos del host al filesystem ZEROS
 *
 * Se ejecuta desde init.c justo después de formatear un disco nuevo,
 * para dejar las fuentes del sistema disponibles desde el primer arranque.
 *
 * Uso:
 *   zeros_populate <disk> <zeros_dest1> <linux_src1> [<zeros_dest2> <linux_src2> ...]
 *
 * Ejemplo:
 *   zeros_populate /dev/sda \
 *       /sys/src/shell/shell.c /sys/src/shell/shell.c \
 *       /sys/src/fs/zeros_fs.h /sys/src/fs/zeros_fs.h
 *
 * Los directorios destino deben existir ya en el disco (los crea
 * zeros_format a través de ZEROS_INITIAL_DIRS). Si no existen, se
 * intenta crearlos; los errores se muestran pero no detienen el proceso.
 */

#define _POSIX_C_SOURCE 200809L

#include "zeros_mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Crear directorios en cadena ─────────────────────────
 *
 * Para una ruta como /sys/src/shell/shell.c, crea:
 *   /sys, /sys/src, /sys/src/shell
 * zeros_mkdir imprime un error si el directorio ya existe,
 * así que suprimimos stderr durante las llamadas: los directorios
 * ya creados por zeros_format no generan ruido en pantalla.
 */
static void mkdirs_p(zeros_mount_t *mnt, const char *zeros_path) {
    char buf[256];
    strncpy(buf, zeros_path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Suprimir stderr temporalmente */
    int saved_err = dup(STDERR_FILENO);
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, STDERR_FILENO);

    /* Recorre componente a componente hasta el penúltimo (el archivo) */
    char *p = buf + 1;   /* saltar la '/' inicial */
    while (*p) {
        char *slash = strchr(p, '/');
        if (!slash) break;          /* llegamos al nombre del archivo */
        *slash = '\0';
        zeros_mkdir(mnt, buf);      /* ignora el error si ya existe */
        *slash = '/';
        p = slash + 1;
    }

    /* Restaurar stderr */
    if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
    if (devnull   >= 0) close(devnull);
}

/* ── Leer un archivo del sistema Linux (initramfs) ───────*/
static uint8_t *read_linux_file(const char *path, uint32_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);

    if (len <= 0 || len > 8 * 1024 * 1024) {
        fprintf(stderr, "zeros_populate: '%s' vacío o demasiado grande\n", path);
        fclose(f);
        return NULL;
    }

    uint8_t *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = (uint32_t)n;
    return buf;
}

int main(int argc, char *argv[]) {
    if (argc < 4 || (argc % 2) != 0) {
        fprintf(stderr,
            "Uso: %s <disco> <zeros_dest> <linux_src> [...]\n"
            "  Copia cada <linux_src> al disco ZEROS en <zeros_dest>.\n",
            argv[0]);
        return 1;
    }

    zeros_mount_t *mnt = zeros_mount_open(argv[1]);
    if (!mnt) {
        fprintf(stderr, "zeros_populate: no se puede abrir '%s'\n", argv[1]);
        return 1;
    }

    int ok = 0, fail = 0;

    for (int i = 2; i < argc; i += 2) {
        const char *zeros_dest = argv[i];
        const char *linux_src  = argv[i + 1];

        uint32_t len;
        uint8_t *buf = read_linux_file(linux_src, &len);
        if (!buf) { fail++; continue; }

        /* Asegurar que los directorios padre existen */
        mkdirs_p(mnt, zeros_dest);

        /* Crear el archivo y escribir contenido */
        zeros_touch(mnt, zeros_dest);
        if (zeros_write_file(mnt, zeros_dest, buf, len) == 0) {
            printf("  [OK] %s\n", zeros_dest);
            ok++;
        } else {
            fprintf(stderr, "  [ERR] %s\n", zeros_dest);
            fail++;
        }
        free(buf);
    }

    zeros_mount_close(mnt);

    printf("  %d copiados, %d errores\n", ok, fail);
    return fail ? 1 : 0;
}
