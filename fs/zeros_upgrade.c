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
#define TMP_INC     "/tmp/_zeros_inc"
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

/* ── Extraer todos los .h de /sys/src/ a TMP_INC ────────
 *
 * TCC necesita encontrar los headers. Los extraemos a un directorio
 * temporal plano y pasamos -I<TMP_INC> al compilador.
 * Solo buscamos un nivel de profundidad en /sys/src/fs/ y /sys/src/shell/
 */
static void extract_headers(zeros_mount_t *mnt) {
    mkdir(TMP_INC, 0755);

    /* Lista de headers conocidos — se amplía con el MANIFEST */
    static const char *headers[] = {
        "/sys/src/fs/zeros_fs.h",
        "/sys/src/fs/zeros_mount.h",
        "/sys/src/fs/vfs.h",
        "/sys/src/shell/editor.h",
        NULL
    };
    for (int i = 0; headers[i]; i++) {
        const char *name = strrchr(headers[i], '/') + 1;
        char dst[256];
        snprintf(dst, sizeof(dst), "%s/%s", TMP_INC, name);
        vfs_extract(mnt, headers[i], dst);
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
    char  tmp_srcs[MAX_ARGS][128];
    int   tcc_argc = 0;
    int   src_idx  = 0;

    tcc_argv[tcc_argc++] = "tcc";
    tcc_argv[tcc_argc++] = "-I" TMP_INC;

    for (int i = 1; i < n; i++) {
        if (tokens[i][0] == '-') {
            /* Flag del compilador — pasar directo */
            tcc_argv[tcc_argc++] = tokens[i];
        } else {
            /* Fuente .c: extraer del VFS a /tmp y añadir al comando */
            char vfs_src[256];
            snprintf(vfs_src, sizeof(vfs_src), "%s/%s", SRC_PREFIX, tokens[i]);
            snprintf(tmp_srcs[src_idx], sizeof(tmp_srcs[src_idx]),
                     "/tmp/_zeros_src_%d.c", src_idx);

            if (vfs_extract(mnt, vfs_src, tmp_srcs[src_idx]) < 0) {
                fprintf(stderr, "    no se pudo extraer '%s'\n", vfs_src);
                return -1;
            }
            tcc_argv[tcc_argc++] = tmp_srcs[src_idx];
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

    /* Limpiar fuentes temporales */
    for (int i = 0; i < src_idx; i++) unlink(tmp_srcs[i]);

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
    return fail ? 1 : 0;
}
