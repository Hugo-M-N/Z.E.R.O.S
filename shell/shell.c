/* Activa extensiones POSIX (strdup, access, opendir...) con -std=c11 */
#define _POSIX_C_SOURCE 200809L

/*
 * Z.E.R.O.S Shell — v0.4
 * Shell minimalista para aprender los fundamentos del sistema.
 *
 * Uso:
 *   ./zeros               — shell sobre el filesystem del host (modo desarrollo)
 *   ./zeros <imagen.img>  — shell con el filesystem ZEROS montado (modo OS)
 *
 * Conceptos cubiertos en este archivo:
 *   - REPL: bucle leer → parsear → ejecutar
 *   - fork() + execvp(): cómo el OS lanza procesos
 *   - waitpid(): esperar a que el hijo termine
 *   - signal(): interceptar Ctrl+C sin morir
 *   - VFS (Virtual File System): capa de abstracción que unifica el
 *     backend ZEROS y el backend host bajo una misma interfaz (vfs.h)
 *   - readline(): lectura de línea con edición y historial
 *   - Autocompletado: comandos del PATH + built-ins + archivos del VFS
 *
 * v0.4: se reemplaza zeros_mount_t* por vfs_t* (driver VFS).
 *       La shell ya no necesita saber qué filesystem hay debajo.
 */

#include <stdio.h>               /* printf, perror */
#include <stdlib.h>              /* exit, free, getenv */
#include <string.h>              /* strtok, strcmp, strdup, strncmp */
#include <unistd.h>              /* fork, execvp, chdir, getcwd, access, usleep */
#include <fcntl.h>               /* open, O_RDWR */
#include <sys/wait.h>            /* waitpid */
#include <sys/reboot.h>          /* reboot, RB_POWER_OFF */
#include <signal.h>              /* signal, SIGINT, kill */
#include <dirent.h>              /* opendir, readdir, closedir */
#include <errno.h>               /* errno, ENOENT */
#include <sys/stat.h>            /* chmod, stat */
#include <time.h>                /* nanosleep */
#include <readline/readline.h>   /* readline() */
#include <readline/history.h>    /* add_history() */
#include <stdint.h>              /* uint32_t */
#include "../fs/vfs.h"           /* driver VFS — abstracción del filesystem */
#include "editor.h"              /* editor de texto integrado */

/* ── Constantes ─────────────────────────────────────────── */

#define MAX_INPUT   1024
#define MAX_ARGS    64

/* ── Driver VFS activo ───────────────────────────────────────
 *
 * g_vfs es siempre no-NULL después de main().
 * g_vfs->is_host indica si estamos sobre el filesystem del sistema
 * o sobre una imagen ZEROS propia.
 */
static vfs_t *g_vfs = NULL;

/* ── Estado FUSE ─────────────────────────────────────────────
 *
 * Cuando la shell arranca con una imagen de disco, zeros_fuse monta
 * esa imagen en g_fuse_mnt. El kernel puede entonces hacer exec()
 * directamente sobre los binarios del disco sin necesidad de /tmp.
 *
 * g_fuse_mnt vacío = no hay FUSE activo (modo host o zeros directo).
 */
static char  g_fuse_mnt[256] = "";  /* mountpoint FUSE; vacío = sin FUSE  */
static pid_t g_fuse_pid       = -1; /* PID del proceso zeros_fuse          */
static char  g_bin_dir[512]   = ""; /* directorio del binario de la shell  */

/* ── Built-ins conocidos (para el autocompletado) ──────── */

static const char *builtins[] = {
    "cd", "exit", "help", "shutdown", "clear",
    "ls", "mkdir", "touch", "cat", "write", "rm", "stat", "edit", "cc",
    NULL
};

/* ── FUSE: montar la imagen de disco ─────────────────────
 *
 * Lanza zeros_fuse en un proceso hijo y espera hasta 2 s a que
 * el mountpoint tenga un st_dev distinto al de /tmp — señal de
 * que FUSE ya está listo para servir peticiones.
 *
 * Busca el binario zeros_fuse en tres lugares, en orden:
 *   1. Mismo directorio que la shell (g_bin_dir)
 *   2. Directorio hermano ../fs/  (layout del repositorio)
 *   3. PATH del sistema
 *
 * Tras el montaje añade fuse_mnt/bin al PATH para que execvp()
 * encuentre los binarios del disco sin necesidad de copiarlos.
 *
 * Devuelve 0 si el montaje fue exitoso, -1 en caso contrario.
 */
static int fuse_try_mount(const char *img_path) {
    strncpy(g_fuse_mnt, "/disk", sizeof(g_fuse_mnt) - 1);
    mkdir(g_fuse_mnt, 0755);  /* crea si no existe (en el host) */

    char try_same[576], try_fs[576];
    snprintf(try_same, sizeof(try_same), "%s/zeros_fuse",       g_bin_dir);
    snprintf(try_fs,   sizeof(try_fs),   "%s/../fs/zeros_fuse", g_bin_dir);

    g_fuse_pid = fork();
    if (g_fuse_pid < 0) {
        perror("zeros: fork zeros_fuse");
        g_fuse_mnt[0] = '\0';
        return -1;
    }
    if (g_fuse_pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        execl(try_same,      "zeros_fuse", img_path, g_fuse_mnt, NULL);
        execl(try_fs,        "zeros_fuse", img_path, g_fuse_mnt, NULL);
        execlp("zeros_fuse", "zeros_fuse", img_path, g_fuse_mnt, NULL);
        _exit(1);
    }

    /* Sondear hasta 2 s comparando st_dev del mountpoint con /tmp */
    struct stat mnt_st, tmp_st;
    stat("/tmp", &tmp_st);
    int mounted = 0;
    for (int i = 0; i < 20; i++) {
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 100000000}, NULL); /* 100 ms */
        if (stat(g_fuse_mnt, &mnt_st) == 0 && mnt_st.st_dev != tmp_st.st_dev) {
            mounted = 1;
            break;
        }
    }

    if (!mounted) {
        /* Solo avisar si zeros_fuse existe pero falló — no si simplemente no está */
        if (access(try_same, X_OK) == 0 || access(try_fs, X_OK) == 0)
            fprintf(stderr, "zeros: zeros_fuse encontrado pero no montó\n");
        kill(g_fuse_pid, SIGTERM);
        waitpid(g_fuse_pid, NULL, 0);
        rmdir(g_fuse_mnt);
        g_fuse_mnt[0] = '\0';
        g_fuse_pid = -1;
        return -1;
    }

    /* Añadir mountpoint/bin al frente del PATH */
    char *old_path = getenv("PATH");
    char  new_path[4096];
    snprintf(new_path, sizeof(new_path), "%s/bin:%s",
             g_fuse_mnt, old_path ? old_path : "");
    setenv("PATH", new_path, 1);

    return 0;
}

/* ── FUSE: desmontar y limpiar ───────────────────────────
 *
 * Llama a fusermount -u (o fusermount3 -u), espera a que el
 * proceso zeros_fuse termine y borra el directorio temporal.
 */
static void fuse_umount(void) {
    if (g_fuse_mnt[0] == '\0') return;

    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        execlp("fusermount",  "fusermount",  "-u", g_fuse_mnt, NULL);
        execlp("fusermount3", "fusermount3", "-u", g_fuse_mnt, NULL);
        _exit(1);
    }
    if (pid > 0) waitpid(pid, NULL, 0);

    if (g_fuse_pid > 0) {
        waitpid(g_fuse_pid, NULL, 0);
        g_fuse_pid = -1;
    }

    g_fuse_mnt[0] = '\0';
}

/* ── PID del proceso externo en ejecución ────────────────
 *
 * Cuando hay un hijo corriendo, Ctrl+C le manda SIGINT a él.
 * Cuando no hay hijo (estamos en el prompt), redibuja la línea.
 */
static volatile pid_t g_running_pid = -1;

/* ── Señales ─────────────────────────────────────────────*/
void handle_sigint(int sig) {
    (void)sig;
    if (g_running_pid > 0) {
        kill(g_running_pid, SIGINT);
        return;
    }
    printf("\n");
    rl_on_new_line();
    rl_redisplay();
}

/* ── Parser ──────────────────────────────────────────────*/
int parse(char *line, char **args) {
    int count = 0;
    char *token = strtok(line, " \t\n");
    while (token != NULL && count < MAX_ARGS - 1) {
        args[count++] = token;
        token = strtok(NULL, " \t\n");
    }
    args[count] = NULL;
    return count;
}

/* ── Autocompletado ──────────────────────────────────────
 *
 * complete_command_and_vfs: generador para posición de comando.
 *   Fase 0 — built-ins
 *   Fase 1 — ejecutables del PATH de Linux
 *   Fase 2 — archivos del VFS (ZEROS devuelve matches; host devuelve NULL)
 */
static char *complete_command_and_vfs(const char *text, int state) {
    static int    phase;
    static int    builtin_idx;
    static char **path_dirs;
    static int    dir_idx;
    static DIR   *cur_dir;
    static int    vfs_state;

    if (state == 0) {
        phase       = 0;
        builtin_idx = 0;
        vfs_state   = 0;
        if (cur_dir) { closedir(cur_dir); cur_dir = NULL; }
        if (path_dirs) {
            for (int i = 0; path_dirs[i]; i++) free(path_dirs[i]);
            free(path_dirs);
            path_dirs = NULL;
        }
        dir_idx = 0;
        char *path_env = getenv("PATH");
        if (path_env) {
            char *path_copy = strdup(path_env);
            int count = 0;
            for (char *p = path_copy; *p; p++) if (*p == ':') count++;
            path_dirs = malloc((count + 2) * sizeof(char *));
            int i = 0;
            char *tok = strtok(path_copy, ":");
            while (tok) { path_dirs[i++] = strdup(tok); tok = strtok(NULL, ":"); }
            path_dirs[i] = NULL;
            free(path_copy);
        }
    }

    size_t len = strlen(text);

    /* Fase 0: built-ins */
    while (phase == 0 && builtins[builtin_idx]) {
        const char *b = builtins[builtin_idx++];
        if (strncmp(b, text, len) == 0)
            return strdup(b);
    }
    if (phase == 0) phase = 1;

    /* Fase 1: ejecutables del PATH */
    if (phase == 1 && path_dirs) {
        while (path_dirs[dir_idx]) {
            if (!cur_dir) {
                cur_dir = opendir(path_dirs[dir_idx]);
                if (!cur_dir) { dir_idx++; continue; }
            }
            struct dirent *entry;
            while ((entry = readdir(cur_dir)) != NULL) {
                if (strncmp(entry->d_name, text, len) != 0) continue;
                char full[MAX_INPUT];
                snprintf(full, sizeof(full), "%s/%s", path_dirs[dir_idx], entry->d_name);
                if (access(full, X_OK) == 0)
                    return strdup(entry->d_name);
            }
            closedir(cur_dir);
            cur_dir = NULL;
            dir_idx++;
        }
        phase = 2;
    }

    /* Fase 2: archivos del VFS (solo ZEROS; host devuelve NULL) */
    if (phase == 2) {
        char *match = vfs_path_complete(g_vfs, text, vfs_state++);
        if (match) return match;
    }

    return NULL;
}

static char *vfs_path_complete_wrapper(const char *text, int state) {
    return vfs_path_complete(g_vfs, text, state);
}

static char **zeros_completion(const char *text, int start, int end) {
    (void)end;
    rl_attempted_completion_over = 1;

    if (start == 0)
        return rl_completion_matches(text, complete_command_and_vfs);

    /* Posición de argumento: el backend ZEROS tiene completado propio;
     * el backend host deja que readline haga su completado de archivos. */
    if (!g_vfs->is_host)
        return rl_completion_matches(text, vfs_path_complete_wrapper);

    rl_attempted_completion_over = 0;
    return NULL;
}

/* ══════════════════════════════════════════════════════════
 * Built-ins
 * ══════════════════════════════════════════════════════════ */

int builtin_cd(char **args) {
    const char *path;
    if (args[1]) {
        path = args[1];
    } else if (g_vfs->is_host) {
        path = getenv("HOME");
        if (!path) { fprintf(stderr, "zeros: cd: HOME no definido\n"); return 1; }
    } else {
        path = "/";
    }
    vfs_cd(g_vfs, path);
    return 1;
}

int builtin_exit(char **args) {
    (void)args;
    vfs_destroy(g_vfs);
    fuse_umount();
    printf("Saliendo de ZEROS...\n");
    exit(0);
}

int builtin_clear(char **args) {
    (void)args;
    printf("\033[2J\033[H");
    fflush(stdout);
    return 1;
}

int builtin_shutdown(char **args) {
    (void)args;

    /* TODO: guardar pesos del modelo de IA antes de apagar */
    printf("¿Actualizar modelo antes de apagar? [s/N]: ");
    fflush(stdout);
    char resp[8] = {0};
    if (fgets(resp, sizeof(resp), stdin) && (resp[0] == 's' || resp[0] == 'S')) {
        printf("(TODO: guardado de modelo no implementado aún)\n");
    }

    printf("Apagando el sistema...\n");
    vfs_destroy(g_vfs);
    fuse_umount();
    reboot(RB_POWER_OFF);
    return 1;
}

int builtin_help(char **args) {
    (void)args;
    printf("\n  Z.E.R.O.S Shell v0.4\n");
    printf("  ─────────────────────────────────────────\n");
    printf("  cd [dir]              Cambia de directorio\n");
    printf("  exit                  Sale de la shell\n");
    printf("  shutdown              Apaga el sistema\n");
    printf("  clear                 Limpia la pantalla\n");
    printf("  help                  Muestra esta ayuda\n");
    printf("  Tab                   Autocompleta\n");
    printf("  ↑ ↓                   Historial\n");
    printf("\n  Filesystem (%s):\n", g_vfs->is_host ? "host" : "ZEROS");
    printf("  ls   [path]           Lista directorio\n");
    printf("  mkdir <path>          Crea directorio\n");
    printf("  touch <path>          Crea archivo vacío\n");
    printf("  cat   <path>          Lee archivo\n");
    printf("  write <path> <texto>  Escribe en archivo\n");
    printf("  rm    <path>          Elimina archivo o dir vacío\n");
    printf("  stat  <path>          Info del archivo\n");
    printf("  edit  <path>          Abre el editor de texto\n");
    if (!g_vfs->is_host)
        printf("  cc    <path.c> [-o s] Compila un archivo C con TCC\n");
    printf("\n");
    return 1;
}

/* ── Comandos de filesystem ──────────────────────────────── */

static int builtin_ls(char **args) {
    vfs_ls(g_vfs, args[1]);
    return 1;
}

static int builtin_mkdir(char **args) {
    if (!args[1]) { fprintf(stderr, "uso: mkdir <path>\n"); return 1; }
    vfs_mkdir(g_vfs, args[1]);
    return 1;
}

static int builtin_touch(char **args) {
    if (!args[1]) { fprintf(stderr, "uso: touch <path>\n"); return 1; }
    vfs_touch(g_vfs, args[1]);
    return 1;
}

static int builtin_cat(char **args) {
    if (!args[1]) { fprintf(stderr, "uso: cat <path>\n"); return 1; }
    vfs_cat(g_vfs, args[1]);
    return 1;
}

static int builtin_write(char **args) {
    if (!args[1] || !args[2]) { fprintf(stderr, "uso: write <path> <texto>\n"); return 1; }
    char content[4096] = {0};
    for (int i = 2; args[i]; i++) {
        if (i > 2) strncat(content, " ", sizeof(content) - strlen(content) - 1);
        strncat(content, args[i], sizeof(content) - strlen(content) - 1);
    }
    vfs_write_file(g_vfs, args[1], content, (uint32_t)strlen(content));
    return 1;
}

static int builtin_rm(char **args) {
    if (!args[1]) { fprintf(stderr, "uso: rm <path>\n"); return 1; }
    vfs_rm(g_vfs, args[1]);
    return 1;
}

static int builtin_stat(char **args) {
    if (!args[1]) { fprintf(stderr, "uso: stat <path>\n"); return 1; }
    vfs_stat_path(g_vfs, args[1]);
    return 1;
}

static int builtin_edit(char **args) {
    if (!args[1]) { fprintf(stderr, "uso: edit <path>\n"); return 1; }
    editor_open(g_vfs, args[1]);
    return 1;
}

/* cc <vfs_path> [-o <salida>]
 *
 * Compila un archivo C del VFS con TCC.
 * Solo disponible con backend ZEROS — en modo host usa el cc del sistema.
 *
 * Flujo:
 *   1. Leer el fuente del VFS con vfs_read_file()
 *   2. Volcarlo a un temporal en /tmp
 *   3. Llamar a tcc sobre el temporal
 *   4. Leer el binario resultante y guardarlo en el VFS con vfs_write_file()
 *   5. Borrar los temporales
 *
 * Esto permite que los binarios compilados queden dentro del disco ZEROS
 * y puedan ejecutarse y modificarse desde el propio sistema.
 */
static int builtin_cc(char **args) {
    if (g_vfs->is_host) return 0;  /* en modo host: usar el cc del sistema */
    if (!args[1]) { fprintf(stderr, "uso: cc <archivo.c> [-o <salida>]\n"); return 1; }

    uint32_t src_len;
    uint8_t *src = vfs_read_file(g_vfs, args[1], &src_len);
    if (!src) {
        fprintf(stderr, "cc: no se puede leer '%s'\n", args[1]);
        return 1;
    }

    /* Nombre base sin extensión para el binario de salida */
    const char *base = strrchr(args[1], '/');
    base = base ? base + 1 : args[1];
    char base_noext[256];
    strncpy(base_noext, base, sizeof(base_noext) - 1);
    base_noext[sizeof(base_noext) - 1] = '\0';
    char *dot = strrchr(base_noext, '.');
    if (dot) *dot = '\0';

    /* Temporal del fuente */
    char tmp_src[64];
    snprintf(tmp_src, sizeof(tmp_src), "/tmp/_zeros_cc_%d.c", (int)getpid());
    FILE *f = fopen(tmp_src, "w");
    if (!f) {
        fprintf(stderr, "cc: no se puede crear temporal en /tmp\n");
        free(src); return 1;
    }
    fwrite(src, 1, src_len, f);
    fclose(f);
    free(src);

    /* Temporal del binario */
    char tmp_out[64];
    snprintf(tmp_out, sizeof(tmp_out), "/tmp/_zeros_out_%d", (int)getpid());

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        char *tcc_args[] = {
            "tcc",
            "-I/usr/lib/musl/include",
            "-static",
            tmp_src, "-o", tmp_out,
            NULL
        };
        execvp("tcc", tcc_args);
        perror("cc: tcc no encontrado");
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    unlink(tmp_src);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 1;

    /* Destino en el VFS: -o explícito o misma carpeta que el fuente */
    const char *vfs_out = NULL;
    for (int i = 2; args[i]; i++) {
        if (strcmp(args[i], "-o") == 0 && args[i + 1]) {
            vfs_out = args[i + 1]; break;
        }
    }
    char default_out[512];
    if (!vfs_out) {
        const char *slash = strrchr(args[1], '/');
        if (slash) {
            int dir_len = (int)(slash - args[1]);
            snprintf(default_out, sizeof(default_out), "%.*s/%s", dir_len, args[1], base_noext);
        } else {
            snprintf(default_out, sizeof(default_out), "%s", base_noext);
        }
        vfs_out = default_out;
    }

    /* Leer el binario compilado */
    FILE *fbin = fopen(tmp_out, "rb");
    if (!fbin) {
        fprintf(stderr, "cc: no se puede leer el binario temporal\n");
        unlink(tmp_out); return 1;
    }
    fseek(fbin, 0, SEEK_END);
    long bin_len = ftell(fbin);
    rewind(fbin);
    uint8_t *bin = malloc(bin_len);
    if (!bin) { fclose(fbin); unlink(tmp_out); return 1; }
    fread(bin, 1, bin_len, fbin);
    fclose(fbin);
    unlink(tmp_out);

    /* Guardar el binario y hacerlo ejecutable.
     *
     * Con FUSE activo: escribir directamente al mountpoint para que el
     * kernel pueda hacer exec() de inmediato sin necesitar /tmp.
     *
     * Sin FUSE: escribir al VFS interno (zeros_mount). El binario queda
     * en el disco pero necesita extracción a /tmp para ejecutarse.
     */
    if (g_fuse_mnt[0]) {
        char fuse_out[4096];
        if (vfs_out[0] == '/')
            snprintf(fuse_out, sizeof(fuse_out), "%s%s", g_fuse_mnt, vfs_out);
        else
            snprintf(fuse_out, sizeof(fuse_out), "%s%s/%s",
                     g_fuse_mnt, vfs_pwd(g_vfs), vfs_out);

        FILE *fout = fopen(fuse_out, "wb");
        if (!fout) {
            fprintf(stderr, "cc: no se puede escribir en '%s'\n", fuse_out);
        } else {
            fwrite(bin, 1, bin_len, fout);
            fclose(fout);
            chmod(fuse_out, 0755);
        }
    } else {
        vfs_write_file(g_vfs, vfs_out, bin, (uint32_t)bin_len);
    }
    free(bin);

    printf("cc: compilado → %s\n", vfs_out);
    return 1;
}


/* ── Despachador de built-ins ────────────────────────────
 *
 * Devuelve 1 si el comando fue manejado como built-in.
 * Devuelve 0 si debe pasar a run_external.
 */
int run_builtin(char **args) {
    if (strcmp(args[0], "cd")       == 0) return builtin_cd(args);
    if (strcmp(args[0], "exit")     == 0) return builtin_exit(args);
    if (strcmp(args[0], "help")     == 0) return builtin_help(args);
    if (strcmp(args[0], "shutdown") == 0) return builtin_shutdown(args);
    if (strcmp(args[0], "clear")    == 0) return builtin_clear(args);
    if (strcmp(args[0], "ls")       == 0) return builtin_ls(args);
    if (strcmp(args[0], "mkdir")    == 0) return builtin_mkdir(args);
    if (strcmp(args[0], "touch")    == 0) return builtin_touch(args);
    if (strcmp(args[0], "cat")      == 0) return builtin_cat(args);
    if (strcmp(args[0], "write")    == 0) return builtin_write(args);
    if (strcmp(args[0], "rm")       == 0) return builtin_rm(args);
    if (strcmp(args[0], "stat")     == 0) return builtin_stat(args);
    if (strcmp(args[0], "edit")     == 0) return builtin_edit(args);
    if (strcmp(args[0], "cc")       == 0) return builtin_cc(args);
    return 0;
}

/* ── Ejecución de comandos externos ──────────────────────
 *
 * Con FUSE activo: execvp encuentra los binarios del disco a través
 * del PATH (fuse_mnt/bin está al frente). El kernel hace exec() directo.
 *
 * Sin FUSE (modo zeros directo): si execvp no encuentra el binario en
 * el sistema, se busca en el VFS, se extrae a /tmp y se ejecuta desde ahí.
 * Este camino es el fallback para cuando zeros_fuse no está disponible.
 */
void run_external(char **args) {
    pid_t pid = fork();
    if (pid < 0) { perror("zeros: fork"); return; }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        execvp(args[0], args);

        /* Fallback sin FUSE: extraer binario del VFS a /tmp y ejecutar.
         *
         * Si el comando no contiene '/' buscamos en los directorios del
         * VFS igual que hace execvp con el PATH del sistema. */
        if (errno == ENOENT && !g_vfs->is_host && g_fuse_mnt[0] == '\0') {
            static const char *vfs_path[] = { "/bin", "/usr/bin", NULL };
            uint32_t len;
            uint8_t *bin = NULL;

            if (strchr(args[0], '/')) {
                bin = vfs_read_file(g_vfs, args[0], &len);
            } else {
                for (int i = 0; vfs_path[i] && !bin; i++) {
                    char full[512];
                    snprintf(full, sizeof(full), "%s/%s", vfs_path[i], args[0]);
                    bin = vfs_read_file(g_vfs, full, &len);
                }
            }
            if (bin) {
                char tmp_bin[64];
                snprintf(tmp_bin, sizeof(tmp_bin), "/tmp/_zeros_run_%d", (int)getpid());
                FILE *f = fopen(tmp_bin, "wb");
                if (f) {
                    fwrite(bin, 1, len, f);
                    fclose(f);
                    chmod(tmp_bin, 0755);
                    free(bin);
                    execv(tmp_bin, args);
                    unlink(tmp_bin);
                }
                free(bin);
            }
        }
        perror("zeros");
        exit(1);
    }
    g_running_pid = pid;
    int status;
    waitpid(pid, &status, WUNTRACED);
    g_running_pid = -1;
}

/* ── REPL principal ──────────────────────────────────────*/
int main(int argc, char *argv[]) {
    char *args[MAX_ARGS];
    char  prompt[MAX_INPUT + 16];

    /* Guardar el directorio del binario para localizar zeros_fuse */
    strncpy(g_bin_dir, argv[0], sizeof(g_bin_dir) - 1);
    char *last_slash = strrchr(g_bin_dir, '/');
    if (last_slash) *last_slash = '\0';
    else strncpy(g_bin_dir, ".", sizeof(g_bin_dir) - 1);

    signal(SIGINT, handle_sigint);
    rl_catch_signals = 0;  /* readline no toca señales — las gestiona la shell */
    rl_attempted_completion_function = zeros_completion;

    if (argc == 2) {
        g_vfs = vfs_open_zeros(argv[1]);
        if (!g_vfs) return 1;

        if (fuse_try_mount(argv[1]) == 0)
            printf("Z.E.R.O.S Shell v0.4 — disco '%s' montado (FUSE en %s)\n",
                   argv[1], g_fuse_mnt);
        else
            printf("Z.E.R.O.S Shell v0.4 — disco '%s' (sin FUSE, exec via /tmp)\n",
                   argv[1]);
    } else {
        g_vfs = vfs_open_host();
        if (!g_vfs) return 1;
        printf("Z.E.R.O.S Shell v0.4 — modo host\n");
    }
    printf("Escribe 'help' para ayuda\n\n");

    while (1) {
        snprintf(prompt, sizeof(prompt), "ZEROS:%s> ", vfs_pwd(g_vfs));

        char *line = readline(prompt);
        if (line == NULL) { printf("\n"); break; }
        if (line[0] == '\0') { free(line); continue; }

        add_history(line);
        int n = parse(line, args);
        if (n > 0) {
            if (!run_builtin(args))
                run_external(args);
        }
        free(line);
    }

    vfs_destroy(g_vfs);
    fuse_umount();
    return 0;
}
