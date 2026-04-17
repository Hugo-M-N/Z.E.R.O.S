/*
 * pager.c — Paginador de texto para Z.E.R.O.S
 *
 * Muestra un buffer de texto con scroll interactivo.
 * Si el texto cabe en pantalla, lo imprime directamente.
 *
 * Teclas:
 *   AvPág   → avanzar una línea
 *   RePág   → retroceder una línea
 *   q       → salir
 *
 * Se usan solo AvPág/RePág (no flechas) para no interferir
 * con el historial de readline cuando se vuelva al prompt.
 *
 * Conceptos:
 *   - termios raw mode: desactiva el eco y el buffering de línea
 *   - TIOCGWINSZ: ioctl que devuelve filas y columnas del terminal
 *   - Secuencias ANSI: ESC[2J (borrar), ESC[H (inicio), ESC[7m (inverso)
 */

#define _POSIX_C_SOURCE 200809L

#include "pager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

/* ── Tamaño del terminal ─────────────────────────────────── */

static void get_term_size(int *rows, int *cols) {
    struct winsize ws;
    *rows = 24;
    *cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) *rows = ws.ws_row;
        if (ws.ws_col > 0) *cols = ws.ws_col;
    }
}

/* ── Lectura de teclas ───────────────────────────────────────
 *
 * Solo reconoce AvPág (ESC[6~) y RePág (ESC[5~).
 * Cualquier otra tecla se devuelve como su carácter.
 */
#define KEY_PGDN  0x100
#define KEY_PGUP  0x101

static int read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 'q';
    if (c != '\033') return (int)c;

    unsigned char seq[3] = {0};
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
    if (seq[0] != '[') return '\033';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';

    if (seq[1] == '5') { read(STDIN_FILENO, &seq[2], 1); return KEY_PGUP; }
    if (seq[1] == '6') { read(STDIN_FILENO, &seq[2], 1); return KEY_PGDN; }

    return '\033';
}

/* ── División en líneas ──────────────────────────────────────
 *
 * Devuelve un array de punteros a las líneas dentro de una
 * copia mutable de buf. Liberar: free(lines[0]); free(lines).
 */
static char **split_lines(const char *buf, size_t len, int *nlines) {
    int count = 0;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n') count++;
    if (len > 0 && buf[len - 1] != '\n') count++;

    char **lines = malloc((count + 1) * sizeof(char *));
    if (!lines) return NULL;
    char *copy = malloc(len + 1);
    if (!copy) { free(lines); return NULL; }
    memcpy(copy, buf, len);
    copy[len] = '\0';

    int idx = 0;
    char *p = copy;
    while (*p && idx < count) {
        lines[idx++] = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; }
        else break;
    }
    lines[idx] = NULL;
    *nlines = idx;
    return lines;
}

/* ── Barra de estado ─────────────────────────────────────── */

static void draw_status(int top, int end, int total, int cols) {
    int at_end = (end >= total);
    printf("\033[7m");
    int n;
    if (at_end)
        n = printf("  FIN — línea %d de %d  [q salir]", end, total);
    else
        n = printf("  línea %d de %d  [AvPág avanzar  RePág retroceder  q salir]",
                   end, total);
    /* Rellenar la línea para que el vídeo inverso llegue al borde */
    for (; n < cols; n++) putchar(' ');
    printf("\033[0m");
    fflush(stdout);
    (void)top;
}

/* ── Función principal ───────────────────────────────────── */

void pager_show(const char *buf, size_t len) {
    if (!buf || len == 0) return;

    int rows, cols;
    get_term_size(&rows, &cols);
    int page = rows - 1;   /* líneas de contenido; la última es la barra */
    if (page < 3) page = 3;

    int nlines;
    char **lines = split_lines(buf, len, &nlines);
    if (!lines) return;

    /* Si el texto cabe en pantalla, imprimir sin pager */
    if (nlines <= page) {
        for (int i = 0; i < nlines; i++)
            printf("%s\n", lines[i]);
        free(lines[0]);
        free(lines);
        return;
    }

    /* ── Modo raw ── */
    struct termios orig, raw;
    tcgetattr(STDIN_FILENO, &orig);
    raw = orig;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    int top = 0;

    while (1) {
        int end = top + page;
        if (end > nlines) end = nlines;

        printf("\033[2J\033[H");
        for (int i = top; i < end; i++)
            printf("%s\n", lines[i]);
        draw_status(top, end, nlines, cols);

        int key = read_key();

        if (key == 'q' || key == 'Q') break;

        if (key == KEY_PGDN && end < nlines) top++;
        if (key == KEY_PGUP && top > 0)      top--;
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    printf("\033[2J\033[H");

    free(lines[0]);
    free(lines);
}
