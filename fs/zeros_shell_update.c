/*
 * zeros_shell_update.c — Descarga la shell precompilada desde GitHub
 *
 * La shell (zeros) depende de readline/ncurses compiladas contra glibc,
 * por lo que no puede recompilarse con TCC dentro de la VM.
 * Este programa descarga el binario precompilado directamente.
 *
 * Lee la URL base del repositorio desde /etc/zeros.conf:
 *   REPO_URL=https://raw.githubusercontent.com/Hugo-M-N/Z.E.R.O.S/main
 *
 * Descarga REPO_URL/bin/zeros → /bin/zeros (en la RAM del initramfs).
 * Como init reinicia la shell con fork+exec, al ejecutar 'exit' la
 * próxima ejecución ya usa el binario nuevo.
 *
 * Uso: zeros_shell_update
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define CONF_PATH    "/etc/zeros.conf"
#define DEFAULT_URL  "https://raw.githubusercontent.com/Hugo-M-N/Z.E.R.O.S/main"
#define TMP_FILE     "/tmp/_zeros_shell_new"
#define DEST_BIN     "/bin/zeros"

/* ── Leer REPO_URL de /etc/zeros.conf ──────────────────*/
static void read_repo_url(char *out, size_t sz) {
    strncpy(out, DEFAULT_URL, sz - 1);
    out[sz - 1] = '\0';

    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "REPO_URL=", 9) == 0) {
            strncpy(out, line + 9, sz - 1);
            out[sz - 1] = '\0';
            char *nl = strchr(out, '\n');
            if (nl) *nl = '\0';
            char *cr = strchr(out, '\r');
            if (cr) *cr = '\0';
            break;
        }
    }
    fclose(f);
}

/* ── Descargar URL a archivo con curl ──────────────────*/
static int download(const char *url, const char *dest) {
    unlink(dest);
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {
            "/bin/curl", "-Lf", "--show-error",
            "-o", (char *)dest, (char *)url, NULL
        };
        char *envp[] = {
            "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt", NULL
        };
        execve("/bin/curl", argv, envp);
        perror("zeros_shell_update: no se pudo ejecutar curl");
        _exit(1);
    }
    if (pid < 0) return -1;
    int st;
    waitpid(pid, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

int main(void) {
    char repo_url[512];
    read_repo_url(repo_url, sizeof(repo_url));

    char url[640];
    snprintf(url, sizeof(url), "%s/bin/zeros", repo_url);

    printf("zeros_shell_update: descargando shell desde GitHub...\n");
    printf("  URL: %s\n", url);
    fflush(stdout);

    if (download(url, TMP_FILE) < 0) {
        fprintf(stderr, "zeros_shell_update: error al descargar\n");
        return 1;
    }

    /* Verificar que es un binario ELF válido */
    unsigned char magic[4] = {0};
    int fd = open(TMP_FILE, O_RDONLY);
    if (fd >= 0) { read(fd, magic, 4); close(fd); }
    if (magic[0] != 0x7f || magic[1] != 'E' ||
        magic[2] != 'L'  || magic[3] != 'F') {
        fprintf(stderr, "zeros_shell_update: el archivo descargado no es un binario ELF válido\n");
        unlink(TMP_FILE);
        return 1;
    }

    /* Copiar a /bin/zeros sobreescribiendo el binario en la RAM del initramfs */
    FILE *src = fopen(TMP_FILE, "rb");
    if (!src) { perror("zeros_shell_update: fopen"); unlink(TMP_FILE); return 1; }

    fseek(src, 0, SEEK_END);
    long len = ftell(src);
    rewind(src);

    unsigned char *buf = malloc((size_t)len);
    if (!buf) { fclose(src); unlink(TMP_FILE); return 1; }
    fread(buf, 1, (size_t)len, src);
    fclose(src);
    unlink(TMP_FILE);

    /* Unlink primero: el proceso en ejecución mantiene el inodo viejo,
     * pero la nueva entrada de directorio apunta al fichero nuevo. */
    unlink(DEST_BIN);
    FILE *dst = fopen(DEST_BIN, "wb");
    if (!dst) {
        perror("zeros_shell_update: no se pudo escribir en /bin/zeros");
        free(buf);
        return 1;
    }
    fwrite(buf, 1, (size_t)len, dst);
    fclose(dst);
    free(buf);

    chmod(DEST_BIN, 0755);

    printf("  Shell actualizada (%ld bytes).\n", len);
    printf("  Ejecuta 'exit' para reiniciarla con la versión nueva.\n");
    return 0;
}
