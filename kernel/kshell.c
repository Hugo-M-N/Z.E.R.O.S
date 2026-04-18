/*
 * kshell.c — Shell del kernel Z.E.R.O.S
 *
 * Port de shell.c sin dependencias POSIX:
 *   - readline  → readline_kernel() con input_readchar()
 *   - vfs_*     → kzeros_*
 *   - printf    → console_print / console_print_uint / console_print_hex
 *   - strtok    → k_strtok
 *   - fork/exec → no disponible (todos los comandos son built-ins)
 */

#include "kshell.h"
#include "kzeros.h"
#include "console.h"
#include "input.h"
#include "kstring.h"

#define MAX_LINE 256
#define MAX_ARGS  32

static kzeros_mount_t *g_fs;

/* ── Lectura de línea con echo y backspace ───────────────*/

static void readline_kernel(const char *prompt, char *buf, unsigned int size) {
    console_print(prompt);
    unsigned int pos = 0;
    for (;;) {
        char c = input_readchar();
        if (c == '\n' || c == '\r') {
            console_print("\n");
            break;
        }
        if ((c == '\b' || c == 127) && pos > 0) {
            pos--;
            console_print("\b \b");
            continue;
        }
        if (c >= 32 && pos < size - 1) {
            buf[pos++] = c;
            char s[2] = {c, 0};
            console_print(s);
        }
    }
    buf[pos] = '\0';
}

/* ── Parser ──────────────────────────────────────────────*/

static int parse(char *line, char **args) {
    int count = 0;
    char *tok = k_strtok(line, " \t");
    while (tok && count < MAX_ARGS - 1) {
        args[count++] = tok;
        tok = k_strtok(0, " \t");
    }
    args[count] = NULL;
    return count;
}

/* ── Built-ins ───────────────────────────────────────────*/

static void cmd_help(void) {
    console_print("\n  Z.E.R.O.S Shell\n");
    console_print("  cd [path]             Cambia directorio\n");
    console_print("  ls [path]             Lista directorio\n");
    console_print("  mkdir <path>          Crea directorio\n");
    console_print("  touch <path>          Crea archivo\n");
    console_print("  cat   <path>          Muestra archivo\n");
    console_print("  write <path> <texto>  Escribe en archivo\n");
    console_print("  rm    <path>          Elimina\n");
    console_print("  stat  <path>          Info del archivo\n");
    console_print("  clear                 Limpia pantalla\n");
    console_print("  shutdown              Apaga el sistema\n\n");
}

static void cmd_cd(char **args) {
    const char *path = args[1] ? args[1] : "/";
    if (kzeros_cd(g_fs, path) < 0)
        console_print("cd: no encontrado\n");
}

static void cmd_ls(char **args) {
    if (kzeros_ls(g_fs, args[1]) < 0)
        console_print("ls: error\n");
}

static void cmd_mkdir(char **args) {
    if (!args[1]) { console_print("uso: mkdir <path>\n"); return; }
    if (kzeros_mkdir(g_fs, args[1]) < 0)
        console_print("mkdir: error\n");
}

static void cmd_touch(char **args) {
    if (!args[1]) { console_print("uso: touch <path>\n"); return; }
    if (kzeros_touch(g_fs, args[1]) < 0)
        console_print("touch: error\n");
}

static void cmd_cat(char **args) {
    if (!args[1]) { console_print("uso: cat <path>\n"); return; }
    if (kzeros_cat(g_fs, args[1]) < 0)
        console_print("cat: no encontrado\n");
    else
        console_print("\n");
}

static void cmd_write(char **args) {
    if (!args[1] || !args[2]) { console_print("uso: write <path> <texto>\n"); return; }
    static char content[4096];
    content[0] = '\0';
    for (int i = 2; args[i]; i++) {
        if (i > 2) k_strncat(content, " ", sizeof(content) - k_strlen(content) - 1);
        k_strncat(content, args[i], sizeof(content) - k_strlen(content) - 1);
    }
    if (kzeros_write_file(g_fs, args[1], content, (unsigned int)k_strlen(content)) < 0)
        console_print("write: error\n");
}

static void cmd_rm(char **args) {
    if (!args[1]) { console_print("uso: rm <path>\n"); return; }
    if (kzeros_rm(g_fs, args[1]) < 0)
        console_print("rm: error (¿directorio no vacío?)\n");
}

static void cmd_stat(char **args) {
    if (!args[1]) { console_print("uso: stat <path>\n"); return; }
    zeros_inode inode;
    if (kzeros_getattr(g_fs, args[1], &inode) < 0) {
        console_print("stat: no encontrado\n");
        return;
    }
    console_print("  tipo: ");
    console_print(ZEROS_IS_DIR(inode.mode) ? "directorio\n" : "archivo\n");
    console_print("  size: ");
    console_print_uint((unsigned int)inode.size);
    console_print(" bytes\n");
    console_print("  modo: ");
    console_print_hex(inode.mode);
    console_print("\n");
}

/* ── REPL ────────────────────────────────────────────────*/

void kshell_run(void) {
    console_print("  [DBG] kshell_run iniciada\n");
    g_fs = kzeros_open();
    if (!g_fs) {
        console_print("  [FS] Error montando disco\n");
        for (;;) __asm__ volatile("hlt");  /* proceso no puede retornar */
    }
    console_print("  [OK] Disco ZEROS montado\n");
    console_print("  Escribe 'help' para ver los comandos.\n\n");

    static char  line[MAX_LINE];
    static char  prompt[72];
    char        *args[MAX_ARGS];

    for (;;) {
        prompt[0] = '\0';
        k_strncat(prompt, "ZEROS:", sizeof(prompt) - 1);
        k_strncat(prompt, kzeros_pwd(g_fs), sizeof(prompt) - k_strlen(prompt) - 1);
        k_strncat(prompt, "> ", sizeof(prompt) - k_strlen(prompt) - 1);

        readline_kernel(prompt, line, sizeof(line));
        if (!line[0]) continue;

        int n = parse(line, args);
        if (n == 0) continue;

        if      (k_strcmp(args[0], "help")     == 0) cmd_help();
        else if (k_strcmp(args[0], "clear")    == 0) console_clear();
        else if (k_strcmp(args[0], "cd")       == 0) cmd_cd(args);
        else if (k_strcmp(args[0], "ls")       == 0) cmd_ls(args);
        else if (k_strcmp(args[0], "mkdir")    == 0) cmd_mkdir(args);
        else if (k_strcmp(args[0], "touch")    == 0) cmd_touch(args);
        else if (k_strcmp(args[0], "cat")      == 0) cmd_cat(args);
        else if (k_strcmp(args[0], "write")    == 0) cmd_write(args);
        else if (k_strcmp(args[0], "rm")       == 0) cmd_rm(args);
        else if (k_strcmp(args[0], "stat")     == 0) cmd_stat(args);
        else if (k_strcmp(args[0], "shutdown") == 0) {
            console_print("Apagando...\n");
            kzeros_close(g_fs);
            __asm__ volatile("cli");
            for (;;) __asm__ volatile("hlt");
        }
        else {
            console_print(args[0]);
            console_print(": comando no encontrado\n");
        }
    }
}
