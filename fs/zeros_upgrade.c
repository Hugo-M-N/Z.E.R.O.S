/*
 * zeros_upgrade.c — Recompila las fuentes de Z.E.R.O.S e instala binarios
 *
 * Lee /sys/src/BUILD del disco ZEROS. Cada línea define un binario:
 *   <nombre> <fuente1> [fuente2...] [flags]
 *
 * Para cada entrada:
 *   1. Extrae los .c del VFS a /tmp
 *   2. Extrae los .h del VFS a /tmp/_zeros_inc/ (para que TCC los encuentre)
 *   3. Compila con TCC
 *   4. Escribe el binario resultante en /bin/<nombre> del disco ZEROS
 *
 * Las fuentes se leen de /sys/src/ — es decir, zeros_update debe haberse
 * ejecutado antes para tener las fuentes actualizadas.
 *
 * Uso:
 *   zeros_upgrade <disco>
 *   zeros_upgrade /dev/sda
 */

#define _POSIX_C_SOURCE 200809L

#include "zeros_mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#define BUILD_PATH  "/sys/src/BUILD"
#define SRC_PREFIX  "/sys/src"
#define BIN_PREFIX  "/bin"
#define TMP_BUILD   "/tmp/_zeros_build"   /* árbol de fuentes con estructura */
#define TMP_OUT     "/tmp/_zeros_upgrade_out"
#define MAX_ARGS    64

/* ── Extraer un archivo del VFS al host ─────────────────*/
static int vfs_extract(zeros_mount_t *mnt, const char *vfs_path,
                       const char *host_path) {
    uint32_t len;
    uint8_t *buf = zeros_read_file(mnt, vfs_path, &len);
    if (!buf) return -1;

    FILE *f = fopen(host_path, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    return 0;
}

/* ── Escribir un archivo del host al VFS ────────────────*/
static int host_to_vfs(zeros_mount_t *mnt, const char *host_path,
                       const char *vfs_path) {
    FILE *f = fopen(host_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len <= 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)len, f);
    fclose(f);

    zeros_touch(mnt, vfs_path);
    int r = zeros_write_file(mnt, vfs_path, buf, (uint32_t)len);
    free(buf);
    return r;
}

/* ── Crear directorios recursivamente ───────────────────*/
static void mkdirs(const char *path) {
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* ── Extraer un archivo del VFS manteniendo su ruta ─────
 *
 * vfs_path: ruta en el VFS, ej. /sys/src/fs/vfs.h
 * base:     directorio raíz destino, ej. /tmp/_zeros_build
 * rel:      ruta relativa dentro del base, ej. fs/vfs.h
 *
 * Resultado: base/rel = /tmp/_zeros_build/fs/vfs.h
 * Los includes relativos como "../fs/vfs.h" desde shell/shell.c
 * resuelven correctamente porque la estructura de carpetas es idéntica
 * a la del repositorio.
 */
static int extract_to_build(zeros_mount_t *mnt,
                             const char *vfs_path, const char *rel) {
    char host_path[256];
    snprintf(host_path, sizeof(host_path), "%s/%s", TMP_BUILD, rel);
    /* crear directorio padre */
    char parent[256];
    strncpy(parent, host_path, sizeof(parent) - 1);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = '\0'; mkdirs(parent); }
    return vfs_extract(mnt, vfs_path, host_path);
}

/* ── Extraer headers al árbol de build ──────────────────*/
static void extract_headers(zeros_mount_t *mnt) {
    mkdirs(TMP_BUILD "/fs");
    mkdirs(TMP_BUILD "/shell");

    static const char *headers[] = {
        "/sys/src/fs/zeros_fs.h",
        "/sys/src/fs/zeros_mount.h",
        "/sys/src/fs/vfs.h",
        "/sys/src/shell/editor.h",
        NULL
    };
    for (int i = 0; headers[i]; i++) {
        const char *rel = headers[i] + strlen("/sys/src/");
        extract_to_build(mnt, headers[i], rel);
    }
}

/* ── Compilar una entrada del BUILD ─────────────────────
 *
 * tokens[0]  = nombre del binario destino
 * tokens[1…] = fuentes (.c) y flags (los que empiezan por -)
 *
 * Devuelve 0 si la compilación fue exitosa.
 */
static int compile_entry(zeros_mount_t *mnt, char **tokens, int n) {
    char *tcc_argv[MAX_ARGS + 8];
    char  host_srcs[MAX_ARGS][256];
    int   tcc_argc = 0;
    int   src_idx  = 0;

    tcc_argv[tcc_argc++] = "tcc";
    tcc_argv[tcc_argc++] = "-static";              /* musl, no glibc dinámica */
    tcc_argv[tcc_argc++] = "-B/usr/lib/x86_64-linux-gnu"; /* crt1.o + libc.a de musl */
    /* TMP_BUILD mantiene la estructura shell/ fs/ del repo:
     * shell.c en shell/shell.c puede hacer #include "../fs/vfs.h"
     * y TCC lo resuelve correctamente porque fs/vfs.h existe en el árbol */
    tcc_argv[tcc_argc++] = "-I" TMP_BUILD;
    tcc_argv[tcc_argc++] = "-I/usr/lib/musl/include";
    tcc_argv[tcc_argc++] = "-I/usr/lib/tcc/include";

    for (int i = 1; i < n; i++) {
        if (tokens[i][0] == '-') {
            tcc_argv[tcc_argc++] = tokens[i];
        } else {
            /* Fuente .c: extraer al árbol de build manteniendo su ruta relativa */
            char vfs_src[256];
            snprintf(vfs_src, sizeof(vfs_src), "%s/%s", SRC_PREFIX, tokens[i]);
            snprintf(host_srcs[src_idx], sizeof(host_srcs[src_idx]),
                     "%s/%s", TMP_BUILD, tokens[i]);

            if (extract_to_build(mnt, vfs_src, tokens[i]) < 0) {
                fprintf(stderr, "    no se pudo extraer '%s'\n", vfs_src);
                return -1;
            }
            tcc_argv[tcc_argc++] = host_srcs[src_idx];
            src_idx++;
        }
    }

    tcc_argv[tcc_argc++] = "-o";
    tcc_argv[tcc_argc++] = TMP_OUT;
    tcc_argv[tcc_argc]   = NULL;

    unlink(TMP_OUT);

    pid_t pid = fork();
    if (pid == 0) {
        execvp("tcc", tcc_argv);
        perror("zeros_upgrade: tcc no encontrado");
        _exit(1);
    }
    if (pid < 0) return -1;

    int st;
    waitpid(pid, &st, 0);

    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;

    /* Escribir binario al VFS en /bin/<nombre> */
    char vfs_bin[128];
    snprintf(vfs_bin, sizeof(vfs_bin), "%s/%s", BIN_PREFIX, tokens[0]);
    if (host_to_vfs(mnt, TMP_OUT, vfs_bin) < 0) return -1;
    unlink(TMP_OUT);
    return 0;
}

/* ── Main ────────────────────────────────────────────────*/
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <disco>\n"
                        "  Ejemplo: %s /dev/sda\n", argv[0], argv[0]);
        return 1;
    }

    zeros_mount_t *mnt = zeros_mount_open(argv[1]);
    if (!mnt) {
        fprintf(stderr, "zeros_upgrade: no se puede abrir '%s'\n", argv[1]);
        return 1;
    }

    /* Leer BUILD del VFS */
    uint32_t build_len;
    uint8_t *build_buf = zeros_read_file(mnt, BUILD_PATH, &build_len);
    if (!build_buf) {
        fprintf(stderr, "zeros_upgrade: no se encuentra %s\n"
                        "  Ejecuta primero 'zeros_update %s'\n",
                BUILD_PATH, argv[1]);
        zeros_mount_close(mnt);
        return 1;
    }

    /* Extraer headers una vez para todos los binarios */
    extract_headers(mnt);

    int ok = 0, fail = 0;
    char *line = strtok((char *)build_buf, "\n");

    while (line) {
        /* Saltar comentarios y líneas vacías */
        while (*line == ' ' || *line == '\t') line++;
        if (line[0] == '#' || line[0] == '\0') {
            line = strtok(NULL, "\n");
            continue;
        }
        /* Eliminar \r */
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        /* Tokenizar la línea */
        char *tokens[MAX_ARGS];
        int n = 0;
        char *tok = strtok(line, " \t");
        while (tok && n < MAX_ARGS - 1) {
            tokens[n++] = tok;
            tok = strtok(NULL, " \t");
        }
        if (n < 2) { line = strtok(NULL, "\n"); continue; }

        printf("  %-20s ", tokens[0]);
        fflush(stdout);

        if (compile_entry(mnt, tokens, n) == 0) {
            printf("[OK]\n");
            ok++;
        } else {
            printf("[ERROR]\n");
            fail++;
        }

        line = strtok(NULL, "\n");
    }

    free(build_buf);
    zeros_mount_close(mnt);

    printf("\nzeros_upgrade: %d compilados, %d errores\n", ok, fail);
    printf("\n\033[1;33mAviso:\033[0m la shell (zeros) no se recompila aquí — depende de\n");
    printf("readline/ncurses que son incompatibles con musl dentro de la VM.\n");
    printf("Para actualizar la shell ejecuta: \033[1mzeros_shell_update\033[0m\n\n");
    return fail ? 1 : 0;
}
