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
#include "arch/x86/io.h"

#define MAX_LINE 256
#define MAX_ARGS  32

/* ── Lectura de línea con echo y backspace ───────────────*/

static void readline_kernel(const char *prompt, char *buf, unsigned int size) {
    console_print(prompt);
    unsigned int pos = 0;
    for (;;) {
        unsigned char c = (unsigned char)input_readchar();
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
            buf[pos++] = (char)c;
            char s[2] = {(char)c, 0};
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

static void cmd_cd(kzeros_mount_t *fs, char **args) {
    const char *path = args[1] ? args[1] : "/";
    if (kzeros_cd(fs, path) < 0)
        console_print("cd: no encontrado\n");
}

static void cmd_ls(kzeros_mount_t *fs, char **args) {
    if (kzeros_ls(fs, args[1]) < 0)
        console_print("ls: error\n");
}

static void cmd_mkdir(kzeros_mount_t *fs, char **args) {
    if (!args[1]) { console_print("uso: mkdir <path>\n"); return; }
    if (kzeros_mkdir(fs, args[1]) < 0)
        console_print("mkdir: error\n");
}

static void cmd_touch(kzeros_mount_t *fs, char **args) {
    if (!args[1]) { console_print("uso: touch <path>\n"); return; }
    if (kzeros_touch(fs, args[1]) < 0)
        console_print("touch: error\n");
}

static void cmd_cat(kzeros_mount_t *fs, char **args) {
    if (!args[1]) { console_print("uso: cat <path>\n"); return; }
    if (kzeros_cat(fs, args[1]) < 0)
        console_print("cat: no encontrado\n");
    else
        console_print("\n");
}

static void cmd_write(kzeros_mount_t *fs, char **args) {
    if (!args[1] || !args[2]) { console_print("uso: write <path> <texto>\n"); return; }
    static char content[4096];
    content[0] = '\0';
    for (int i = 2; args[i]; i++) {
        if (i > 2) k_strncat(content, " ", sizeof(content) - k_strlen(content) - 1);
        k_strncat(content, args[i], sizeof(content) - k_strlen(content) - 1);
    }
    if (kzeros_write_file(fs, args[1], content, (unsigned int)k_strlen(content)) < 0)
        console_print("write: error\n");
}

static void cmd_rm(kzeros_mount_t *fs, char **args) {
    if (!args[1]) { console_print("uso: rm <path>\n"); return; }
    if (kzeros_rm(fs, args[1]) < 0)
        console_print("rm: error (¿directorio no vacío?)\n");
}

static void cmd_stat(kzeros_mount_t *fs, char **args) {
    if (!args[1]) { console_print("uso: stat <path>\n"); return; }
    zeros_inode inode;
    if (kzeros_getattr(fs, args[1], &inode) < 0) {
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
    kzeros_mount_t *fs = kzeros_open();
    if (!fs) {
        console_print("  [FS] Error al montar disco ZEROS\n");
        for (;;) __asm__ volatile("hlt");
    }
    console_print("  [OK] Disco ZEROS montado\n");
    console_print("  Escribe 'help' para ver los comandos.\n\n");

    char  line[MAX_LINE];
    char  prompt[72];
    char *args[MAX_ARGS];

    for (;;) {
        prompt[0] = '\0';
        k_strncat(prompt, "ZEROS:", sizeof(prompt) - 1);
        k_strncat(prompt, kzeros_pwd(fs), sizeof(prompt) - k_strlen(prompt) - 1);
        k_strncat(prompt, "> ", sizeof(prompt) - k_strlen(prompt) - 1);

        readline_kernel(prompt, line, sizeof(line));
        if (!line[0]) continue;

        int n = parse(line, args);
        if (n == 0) continue;

        if      (k_strcmp(args[0], "help")     == 0) cmd_help();
        else if (k_strcmp(args[0], "clear")    == 0) console_clear();
        else if (k_strcmp(args[0], "cd")       == 0) cmd_cd(fs, args);
        else if (k_strcmp(args[0], "ls")       == 0) cmd_ls(fs, args);
        else if (k_strcmp(args[0], "mkdir")    == 0) cmd_mkdir(fs, args);
        else if (k_strcmp(args[0], "touch")    == 0) cmd_touch(fs, args);
        else if (k_strcmp(args[0], "cat")      == 0) cmd_cat(fs, args);
        else if (k_strcmp(args[0], "write")    == 0) cmd_write(fs, args);
        else if (k_strcmp(args[0], "rm")       == 0) cmd_rm(fs, args);
        else if (k_strcmp(args[0], "stat")     == 0) cmd_stat(fs, args);
        else if (k_strcmp(args[0], "shutdown") == 0) {
            console_print("Apagando...\n");
            kzeros_close(fs);
            __asm__ volatile("cli");
            /* ACPI S5 (soft-off) — VirtualBox: PM1a_CNT en 0x4004 */
            outw(0x4004, 0x3400);
            /* Fallback: QEMU */
            outw(0x0604, 0x2000);
            for (;;) __asm__ volatile("hlt");
        }
        else {
            console_print(args[0]);
            console_print(": comando no encontrado\n");
        }
    }
}
