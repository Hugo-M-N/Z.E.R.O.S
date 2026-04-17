/*
 * zeros_update.c — Actualiza las fuentes de Z.E.R.O.S desde GitHub
 *
 * Lee la URL base del repositorio desde /etc/zeros.conf:
 *   REPO_URL=https://raw.githubusercontent.com/Hugo-M-N/Z.E.R.O.S/main
 *
 * Descarga REPO_URL/MANIFEST — un archivo de texto con una ruta por línea:
 *   shell/shell.c
 *   fs/zeros_mount.c
 *   ...
 *
 * Por cada ruta del MANIFEST descarga el archivo y lo guarda en
 * /sys/src/<ruta> dentro del disco ZEROS.
 *
 * Para añadir un archivo nuevo al sistema basta con añadirlo al MANIFEST
 * del repo — no hace falta recompilar zeros_update.
 *
 * Uso:
 *   zeros_update <disco>
 *   zeros_update /dev/sda
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

#define CONF_PATH     "/etc/zeros.conf"
#define DEFAULT_URL   "https://raw.githubusercontent.com/Hugo-M-N/Z.E.R.O.S/main"
#define TMP_FILE      "/tmp/_zeros_update_tmp"
#define TMP_MANIFEST  "/tmp/_zeros_manifest"
#define SRC_PREFIX    "/sys/src"

/* ── Leer REPO_URL de /etc/zeros.conf ───────────────────
 *
 * Si no existe el archivo o no tiene la clave, usa DEFAULT_URL.
 */
static void read_repo_url(zeros_mount_t *mnt, char *out, size_t sz) {
    strncpy(out, DEFAULT_URL, sz - 1);
    out[sz - 1] = '\0';

    uint32_t len;
    uint8_t *conf = zeros_read_file(mnt, CONF_PATH, &len);
    if (!conf) return;

    char *line = strtok((char *)conf, "\n");
    while (line) {
        if (strncmp(line, "REPO_URL=", 9) == 0) {
            strncpy(out, line + 9, sz - 1);
            out[sz - 1] = '\0';
            char *cr = strchr(out, '\r');
            if (cr) *cr = '\0';
            break;
        }
        line = strtok(NULL, "\n");
    }
    free(conf);
}

/* ── Descargar una URL a un archivo del host con wget ───*/
static int wget(const char *url, const char *out_path) {
    unlink(out_path);
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        char *argv[] = {
            "/bin/busybox", "wget", "-O", (char *)out_path, (char *)url, NULL
        };
        char *envp[] = { NULL };
        execve("/bin/busybox", argv, envp);
        _exit(1);
    }
    if (pid < 0) return -1;
    int st;
    waitpid(pid, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* ── Copiar archivo del host al VFS ─────────────────────*/
static int copy_to_vfs(zeros_mount_t *mnt, const char *src, const char *dest) {
    FILE *f = fopen(src, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len <= 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)len, f);
    fclose(f);

    zeros_touch(mnt, dest);
    int r = zeros_write_file(mnt, dest, buf, (uint32_t)len);
    free(buf);
    return r;
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
        fprintf(stderr, "zeros_update: no se puede abrir '%s'\n", argv[1]);
        return 1;
    }

    char repo_url[512];
    read_repo_url(mnt, repo_url, sizeof(repo_url));
    printf("zeros_update: repo → %s\n\n", repo_url);

    /* 1. Descargar el MANIFEST */
    char manifest_url[640];
    snprintf(manifest_url, sizeof(manifest_url), "%s/MANIFEST", repo_url);
    printf("  Descargando MANIFEST... ");
    fflush(stdout);
    if (wget(manifest_url, TMP_MANIFEST) < 0) {
        printf("[ERROR]\n");
        fprintf(stderr, "zeros_update: no se pudo descargar el MANIFEST\n");
        zeros_mount_close(mnt);
        return 1;
    }
    printf("[OK]\n\n");

    /* 2. Leer el MANIFEST línea a línea y descargar cada archivo */
    FILE *manifest = fopen(TMP_MANIFEST, "r");
    if (!manifest) {
        fprintf(stderr, "zeros_update: no se puede leer el MANIFEST\n");
        zeros_mount_close(mnt);
        return 1;
    }

    int ok = 0, fail = 0;
    char repo_path[256];

    while (fgets(repo_path, sizeof(repo_path), manifest)) {
        /* Eliminar salto de línea y retorno de carro */
        repo_path[strcspn(repo_path, "\r\n")] = '\0';
        if (repo_path[0] == '\0') continue;  /* línea vacía */

        /* URL de descarga y destino en el VFS */
        char url[768], vfs_dest[512];
        snprintf(url,      sizeof(url),      "%s/%s",        repo_url,   repo_path);
        snprintf(vfs_dest, sizeof(vfs_dest), "%s/%s",        SRC_PREFIX, repo_path);

        printf("  %-40s ", repo_path);
        fflush(stdout);

        if (wget(url, TMP_FILE) < 0) {
            printf("[ERROR descarga]\n");
            fail++;
            continue;
        }
        if (copy_to_vfs(mnt, TMP_FILE, vfs_dest) < 0) {
            printf("[ERROR escritura]\n");
            fail++;
            continue;
        }
        printf("[OK]\n");
        ok++;
    }

    fclose(manifest);
    unlink(TMP_MANIFEST);
    unlink(TMP_FILE);
    zeros_mount_close(mnt);

    printf("\nzeros_update: %d actualizados, %d errores\n", ok, fail);
    if (ok > 0)
        printf("Ejecuta 'zeros_upgrade %s' para recompilar.\n", argv[1]);
    return fail ? 1 : 0;
}
