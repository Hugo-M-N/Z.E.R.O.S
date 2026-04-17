/*
 * editor.c — Editor de texto minimalista para Z.E.R.O.S
 *
 * Inspirado en kilo (antirez), adaptado al filesystem ZEROS.
 *
 * Controles:
 *   Flechas          Mover cursor
 *   Inicio / Fin     Principio / fin de línea
 *   Re Pág / Av Pág  Scroll rápido
 *   Backspace / Del  Borrar carácter
 *   Enter            Nueva línea (con auto-indentación)
 *   Tab              4 espacios
 *   Ctrl+S           Guardar
 *   Ctrl+Q           Salir (pide confirmación si hay cambios)
 *   Ctrl+F           Buscar  (↓/↑ siguiente/anterior, ESC cancelar)
 *   Ctrl+Z           Deshacer (hasta 20 niveles)
 *
 * Conceptos nuevos cubiertos:
 *   - termios: control del terminal a bajo nivel
 *   - Modo raw: leer tecla a tecla sin buffering ni echo
 *   - Secuencias de escape ANSI: mover cursor, limpiar pantalla,
 *     invertir colores desde código C
 *   - ioctl(TIOCGWINSZ): obtener tamaño real del terminal
 */

#define _POSIX_C_SOURCE 200809L

#include "editor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>     /* tcgetattr, tcsetattr, struct termios */
#include <sys/ioctl.h>   /* ioctl, TIOCGWINSZ, struct winsize */

/* ── Teclas especiales ───────────────────────────────────
 *
 * Las flechas y otras teclas de función llegan como secuencias
 * de escape: ESC [ A (flecha arriba), ESC [ B (abajo), etc.
 * Las mapeamos a enteros > 127 para distinguirlas de ASCII.
 */
#define KEY_UP       1000
#define KEY_DOWN     1001
#define KEY_LEFT     1002
#define KEY_RIGHT    1003
#define KEY_HOME     1004
#define KEY_END      1005
#define KEY_PAGE_UP  1006
#define KEY_PAGE_DN  1007
#define KEY_DEL      1008
#define KEY_BACKSP   127

/* Ctrl+letra: pone los bits 6 y 7 a 0.
 * Usamos CTRL_KEY para no colisionar con sys/ttydefaults.h */
#define CTRL_KEY(k)  ((k) & 0x1f)

/* ── Deshacer ────────────────────────────────────────────
 *
 * Guardamos snapshots del contenido completo (texto serializado
 * con '\n') antes de cada operación destructiva.
 * Hasta UNDO_MAX niveles; cuando se llena, descartamos el más antiguo.
 */
#define UNDO_MAX 50
typedef struct {
    char *text;   /* contenido serializado con '\n' entre líneas */
    int   len;
    int   cx, cy; /* posición del cursor en el momento del snapshot */
} UndoSnap;

/* ── Línea ───────────────────────────────────────────────*/
typedef struct {
    char *data;
    int   len;
    int   cap;
} Line;

/* ── Estado del editor ───────────────────────────────────*/
typedef struct {
    /* Contenido */
    Line  *lines;
    int    nlines;
    int    lines_cap;

    /* Cursor y scroll */
    int cx, cy;        /* columna y fila del cursor (en el texto) */
    int rowoff;        /* primera fila visible */
    int coloff;        /* primera columna visible */

    /* Terminal */
    int screenrows;    /* filas de pantalla disponibles para texto */
    int screencols;    /* columnas */

    /* Estado */
    int  dirty;        /* cambios sin guardar */
    int  quit_confirm; /* veces que hay que pulsar Ctrl+Q si dirty */
    int  should_quit;

    /* Archivo */
    char filename[256];

    /* Mensaje de estado */
    char statusmsg[256];

    /* Filesystem */
    vfs_t *vfs;

    /* Deshacer */
    UndoSnap undo_stack[UNDO_MAX];
    int      undo_count;
} EditorState;

/* ── Modo raw ────────────────────────────────────────────
 *
 * En modo normal el terminal procesa las teclas antes de
 * dárselas al programa: Enter envía la línea, Ctrl+C mata
 * el proceso, etc. En modo raw el programa recibe cada
 * pulsación directamente sin procesamiento.
 */
static struct termios s_orig_termios;

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_orig_termios);
}

static int enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &s_orig_termios) < 0) return -1;

    struct termios raw = s_orig_termios;

    /* Flags de entrada:
     *   IXON   — deshabilita Ctrl+S / Ctrl+Q como control de flujo
     *   ICRNL  — deshabilita traducir \r a \n
     *   BRKINT, INPCK, ISTRIP — flags obsoletos, buena práctica desactivarlos */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    /* Flags de salida:
     *   OPOST  — deshabilita procesamiento de salida (\n → \r\n automático) */
    raw.c_oflag &= ~(OPOST);

    /* Flags de control: 8 bits por carácter */
    raw.c_cflag |= CS8;

    /* Flags locales:
     *   ECHO   — no mostrar lo que se escribe
     *   ICANON — desactiva modo canónico (lectura línea a línea)
     *   ISIG   — desactiva Ctrl+C / Ctrl+Z
     *   IEXTEN — desactiva Ctrl+V */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    /* VMIN=0, VTIME=1: read() devuelve tan pronto haya datos
     * o tras 100 ms sin datos (para no bloquear indefinidamente) */
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;

    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* ── Tamaño del terminal ─────────────────────────────────*/
static int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0) {
        *rows = 24; *cols = 80;  /* fallback */
        return 0;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

/* ── Lectura de teclas ───────────────────────────────────
 *
 * Lee un carácter. Si es ESC, intenta leer la secuencia
 * de escape completa para identificar flechas y teclas especiales.
 */
static int read_key(void) {
    char c;
    while (read(STDIN_FILENO, &c, 1) != 1) {
        if (errno != EAGAIN) return -1;
    }

    if (c != '\033') return (unsigned char)c;

    /* Secuencia de escape: leer los siguientes bytes */
    char seq[4];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\033';
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': return KEY_HOME;
                    case '3': return KEY_DEL;
                    case '4': return KEY_END;
                    case '5': return KEY_PAGE_UP;
                    case '6': return KEY_PAGE_DN;
                    case '7': return KEY_HOME;
                    case '8': return KEY_END;
                }
            }
        } else {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
        }
    }
    return '\033';
}

/* ── Buffer de renderizado ───────────────────────────────
 *
 * En lugar de hacer write() una vez por carácter (lento y con
 * parpadeo), acumulamos todo en un buffer y lo volcamos de golpe.
 */
typedef struct { char *data; int len; int cap; } RBuf;

static void rb_append(RBuf *rb, const char *s, int len) {
    if (rb->len + len + 1 > rb->cap) {
        rb->cap = rb->len + len + 512;
        rb->data = realloc(rb->data, rb->cap);
    }
    memcpy(rb->data + rb->len, s, len);
    rb->len += len;
}

/* ── Operaciones sobre líneas ────────────────────────────*/

static void line_ensure_cap(Line *l, int needed) {
    if (needed >= l->cap) {
        l->cap = needed + 64;
        l->data = realloc(l->data, l->cap);
    }
}

static void line_insert_char(Line *l, int at, char c) {
    if (at < 0 || at > l->len) at = l->len;
    line_ensure_cap(l, l->len + 2);
    memmove(l->data + at + 1, l->data + at, l->len - at);
    l->data[at] = c;
    l->len++;
    l->data[l->len] = '\0';
}

static void line_delete_char(Line *l, int at) {
    if (at < 0 || at >= l->len) return;
    memmove(l->data + at, l->data + at + 1, l->len - at);
    l->len--;
}

static void line_append(Line *l, const char *s, int len) {
    line_ensure_cap(l, l->len + len + 1);
    memcpy(l->data + l->len, s, len);
    l->len += len;
    l->data[l->len] = '\0';
}

/* ── Operaciones sobre el buffer del editor ─────────────*/

static void editor_append_line(EditorState *e, const char *s, int len) {
    if (e->nlines >= e->lines_cap) {
        e->lines_cap = e->nlines + 16;
        e->lines = realloc(e->lines, e->lines_cap * sizeof(Line));
    }
    Line *l = &e->lines[e->nlines];
    l->cap  = len + 64;
    l->data = malloc(l->cap);
    l->len  = 0;
    l->data[0] = '\0';
    line_append(l, s, len);
    e->nlines++;
}

static void editor_insert_char(EditorState *e, char c) {
    if (e->cy == e->nlines) editor_append_line(e, "", 0);
    line_insert_char(&e->lines[e->cy], e->cx, c);
    e->cx++;
    e->dirty = 1;
}

/* Devuelve el número de espacios/tabs de indentación al inicio de la línea */
static int line_indent_len(Line *l) {
    int i = 0;
    while (i < l->len && (l->data[i] == ' ' || l->data[i] == '\t'))
        i++;
    return i;
}

static void editor_insert_newline(EditorState *e) {
    /* Calculamos la indentación de la línea actual antes de dividirla */
    int indent = 0;
    char indent_buf[256] = {0};
    if (e->cy < e->nlines) {
        indent = line_indent_len(&e->lines[e->cy]);
        if (indent > (int)sizeof(indent_buf) - 1)
            indent = (int)sizeof(indent_buf) - 1;
        memcpy(indent_buf, e->lines[e->cy].data, indent);
    }

    if (e->cy == e->nlines) {
        editor_append_line(e, indent_buf, indent);
    } else {
        /* Dividimos la línea actual en la posición del cursor */
        Line *cur = &e->lines[e->cy];
        char *rest = cur->data + e->cx;
        int   rest_len = cur->len - e->cx;

        /* Insertamos una línea nueva debajo */
        if (e->nlines >= e->lines_cap) {
            e->lines_cap = e->nlines + 16;
            e->lines = realloc(e->lines, e->lines_cap * sizeof(Line));
        }
        memmove(&e->lines[e->cy + 2], &e->lines[e->cy + 1],
                (e->nlines - e->cy - 1) * sizeof(Line));
        e->nlines++;

        Line *newl = &e->lines[e->cy + 1];
        newl->cap  = indent + rest_len + 64;
        newl->data = malloc(newl->cap);
        newl->len  = 0;
        newl->data[0] = '\0';
        /* Primero la indentación, luego el resto de la línea */
        line_append(newl, indent_buf, indent);
        line_append(newl, rest, rest_len);

        /* Truncamos la línea actual */
        cur->len = e->cx;
        cur->data[cur->len] = '\0';
    }
    e->cy++;
    e->cx = indent;  /* cursor justo después de la indentación */
    e->dirty = 1;
}

static void editor_delete_char(EditorState *e) {
    if (e->cy == e->nlines) return;
    Line *cur = &e->lines[e->cy];

    if (e->cx > 0) {
        /* Borrar el carácter anterior */
        line_delete_char(cur, e->cx - 1);
        e->cx--;
    } else if (e->cy > 0) {
        /* Al inicio de línea: unir con la línea anterior */
        Line *prev = &e->lines[e->cy - 1];
        e->cx = prev->len;
        line_append(prev, cur->data, cur->len);
        free(cur->data);
        memmove(&e->lines[e->cy], &e->lines[e->cy + 1],
                (e->nlines - e->cy - 1) * sizeof(Line));
        e->nlines--;
        e->cy--;
    }
    e->dirty = 1;
}

static void editor_delete_forward(EditorState *e) {
    if (e->cy == e->nlines) return;
    Line *cur = &e->lines[e->cy];
    if (e->cx < cur->len) {
        line_delete_char(cur, e->cx);
        e->dirty = 1;
    } else if (e->cy < e->nlines - 1) {
        /* Al final de línea: unir con la siguiente */
        Line *next = &e->lines[e->cy + 1];
        line_append(cur, next->data, next->len);
        free(next->data);
        memmove(&e->lines[e->cy + 1], &e->lines[e->cy + 2],
                (e->nlines - e->cy - 2) * sizeof(Line));
        e->nlines--;
        e->dirty = 1;
    }
}

/* ── Ajuste de scroll ─────────────────────────────────── */

static void editor_scroll(EditorState *e) {
    if (e->cy < e->rowoff)
        e->rowoff = e->cy;
    if (e->cy >= e->rowoff + e->screenrows)
        e->rowoff = e->cy - e->screenrows + 1;
    if (e->cx < e->coloff)
        e->coloff = e->cx;
    if (e->cx >= e->coloff + e->screencols)
        e->coloff = e->cx - e->screencols + 1;
}

/* ── Ajuste de cursor ─────────────────────────────────── */

static void editor_clamp_cursor(EditorState *e) {
    if (e->cy < 0) e->cy = 0;
    if (e->cy > e->nlines) e->cy = e->nlines;
    int linelen = (e->cy < e->nlines) ? e->lines[e->cy].len : 0;
    if (e->cx < 0) e->cx = 0;
    if (e->cx > linelen) e->cx = linelen;
}

/* ── Renderizado ─────────────────────────────────────────
 *
 * Construye la pantalla completa en un RBuf y la vuelca
 * de una vez para evitar parpadeo.
 *
 * Secuencias ANSI usadas:
 *   \033[?25l / \033[?25h  — ocultar / mostrar cursor
 *   \033[H                 — mover cursor a (1,1)
 *   \033[K                 — borrar hasta fin de línea
 *   \033[7m / \033[m       — texto invertido / reset
 *   \033[%d;%dH            — mover cursor a fila, columna
 */
static void editor_draw(EditorState *e) {
    editor_scroll(e);

    RBuf rb = {NULL, 0, 0};
    char buf[64];

    rb_append(&rb, "\033[?25l", 6);   /* ocultar cursor */
    rb_append(&rb, "\033[H",    3);   /* ir a (1,1) */

    /* ── Filas de contenido ── */
    for (int y = 0; y < e->screenrows; y++) {
        int filerow = y + e->rowoff;
        if (filerow >= e->nlines) {
            rb_append(&rb, "~", 1);
        } else {
            Line *l = &e->lines[filerow];
            int len = l->len - e->coloff;
            if (len < 0) len = 0;
            if (len > e->screencols) len = e->screencols;
            if (len > 0) rb_append(&rb, l->data + e->coloff, len);
        }
        rb_append(&rb, "\033[K\r\n", 5);  /* borrar resto de línea + nueva línea */
    }

    /* ── Barra de estado (fondo invertido) ── */
    rb_append(&rb, "\033[7m", 4);
    int slen = snprintf(buf, sizeof(buf), " %s%s — %d líneas  %d:%d",
                        e->filename[0] ? e->filename : "sin nombre",
                        e->dirty ? " [+]" : "",
                        e->nlines, e->cy + 1, e->cx + 1);
    if (slen > e->screencols) slen = e->screencols;
    rb_append(&rb, buf, slen);
    for (int i = slen; i < e->screencols; i++) rb_append(&rb, " ", 1);
    rb_append(&rb, "\033[m\r\n", 5);

    /* ── Barra de mensajes ── */
    rb_append(&rb, "\033[K", 3);
    const char *hint = (e->statusmsg[0])
                     ? e->statusmsg
                     : "  Ctrl+S guardar  Ctrl+Q salir  Ctrl+F buscar  Ctrl+Z deshacer";
    int hlen = (int)strlen(hint);
    if (hlen > e->screencols) hlen = e->screencols;
    rb_append(&rb, hint, hlen);

    /* ── Posición del cursor ── */
    int row = (e->cy - e->rowoff) + 1;
    int col = (e->cx - e->coloff) + 1;
    int n = snprintf(buf, sizeof(buf), "\033[%d;%dH", row, col);
    rb_append(&rb, buf, n);

    rb_append(&rb, "\033[?25h", 6);   /* mostrar cursor */

    write(STDOUT_FILENO, rb.data, rb.len);
    free(rb.data);
}

/* ── Guardar ─────────────────────────────────────────────*/
static void editor_save(EditorState *e) {
    if (!e->filename[0]) {
        snprintf(e->statusmsg, sizeof(e->statusmsg),
                 "  Sin nombre de archivo — no se puede guardar");
        return;
    }

    /* Calculamos el tamaño total */
    int total = 0;
    for (int i = 0; i < e->nlines; i++) total += e->lines[i].len + 1; /* +1 por \n */

    char *buf = malloc(total + 1);
    if (!buf) { snprintf(e->statusmsg, sizeof(e->statusmsg), "  Error: sin memoria"); return; }

    int off = 0;
    for (int i = 0; i < e->nlines; i++) {
        memcpy(buf + off, e->lines[i].data, e->lines[i].len);
        off += e->lines[i].len;
        buf[off++] = '\n';
    }
    buf[off] = '\0';

    if (vfs_write_file(e->vfs, e->filename, buf, off) == 0) {
        e->dirty = 0;
        e->quit_confirm = 1;
        snprintf(e->statusmsg, sizeof(e->statusmsg),
                 "  Guardado: %d bytes", off);
    } else {
        snprintf(e->statusmsg, sizeof(e->statusmsg),
                 "  Error al guardar");
    }
    free(buf);
}

/* ── Cargar archivo ──────────────────────────────────────
 *
 * Devuelve 0 si el archivo se cargó (o se creó vacío) correctamente.
 * Devuelve -1 si hubo un error de lectura real (archivo corrupto, etc.).
 */
static int editor_load(EditorState *e, const char *path) {
    uint32_t len;
    uint8_t *raw = vfs_read_file(e->vfs, path, &len);

    if (!raw) {
        /* vfs_read_file devuelve NULL si el archivo no existe o hay error.
         * Intentamos crearlo vacío; si falla, es un error real. */
        if (vfs_touch(e->vfs, path) != 0) {
            fprintf(stderr, "editor: no se puede crear '%s'\n", path);
            return -1;
        }
        editor_append_line(e, "", 0);
        return 0;
    }

    /* Dividimos el contenido en líneas por '\n' */
    char *start = (char *)raw;
    char *end   = (char *)raw + len;
    char *p     = start;

    while (p < end) {
        char *nl = memchr(p, '\n', end - p);
        int   line_len = nl ? (int)(nl - p) : (int)(end - p);
        editor_append_line(e, p, line_len);
        p = nl ? nl + 1 : end;
    }
    if (e->nlines == 0) editor_append_line(e, "", 0);

    free(raw);
    e->dirty = 0;
    return 0;
}

/* ── Deshacer: push / pop ────────────────────────────────
 *
 * editor_snap_push serializa todo el contenido en un string
 * (líneas unidas por '\n') y lo apila junto con la posición
 * del cursor. Si la pila está llena descarta el snapshot más
 * antiguo (FIFO).
 *
 * editor_snap_pop restaura el último snapshot y lo elimina
 * de la pila.
 */
static void editor_snap_push(EditorState *e) {
    if (e->nlines == 0) return;

    /* Calcular tamaño serializado */
    int total = 0;
    for (int i = 0; i < e->nlines; i++)
        total += e->lines[i].len + 1;   /* +1 por '\n' */

    char *text = malloc(total + 1);
    if (!text) return;

    int off = 0;
    for (int i = 0; i < e->nlines; i++) {
        memcpy(text + off, e->lines[i].data, e->lines[i].len);
        off += e->lines[i].len;
        text[off++] = '\n';
    }
    text[off] = '\0';

    /* Si la pila está llena, descartar el snapshot más antiguo */
    if (e->undo_count == UNDO_MAX) {
        free(e->undo_stack[0].text);
        memmove(&e->undo_stack[0], &e->undo_stack[1],
                (UNDO_MAX - 1) * sizeof(UndoSnap));
        e->undo_count--;
    }

    e->undo_stack[e->undo_count].text = text;
    e->undo_stack[e->undo_count].len  = off;
    e->undo_stack[e->undo_count].cx   = e->cx;
    e->undo_stack[e->undo_count].cy   = e->cy;
    e->undo_count++;
}

static void editor_snap_pop(EditorState *e) {
    if (e->undo_count == 0) {
        snprintf(e->statusmsg, sizeof(e->statusmsg),
                 "  Nada que deshacer");
        return;
    }

    /* Liberar líneas actuales */
    for (int i = 0; i < e->nlines; i++) free(e->lines[i].data);
    e->nlines = 0;

    /* Restaurar snapshot */
    int idx = --e->undo_count;
    char *text = e->undo_stack[idx].text;
    int   len  = e->undo_stack[idx].len;

    char *p   = text;
    char *end = text + len;
    while (p < end) {
        char *nl   = memchr(p, '\n', end - p);
        int   llen = nl ? (int)(nl - p) : (int)(end - p);
        editor_append_line(e, p, llen);
        p = nl ? nl + 1 : end;
    }
    if (e->nlines == 0) editor_append_line(e, "", 0);

    e->cx = e->undo_stack[idx].cx;
    e->cy = e->undo_stack[idx].cy;
    editor_clamp_cursor(e);
    e->dirty = 1;

    free(text);
    e->undo_stack[idx].text = NULL;

    snprintf(e->statusmsg, sizeof(e->statusmsg),
             "  Deshacer — %d nivel%s restante%s",
             e->undo_count,
             e->undo_count == 1 ? "" : "es",
             e->undo_count == 1 ? "" : "s");
}

/* ── Búsqueda ────────────────────────────────────────────
 *
 * editor_find_next busca la siguiente (dir=1) o anterior (dir=-1)
 * ocurrencia de 'query' a partir del cursor.
 *
 * editor_find muestra un mini-prompt en la barra de mensajes
 * y permite al usuario escribir la cadena a buscar.
 * Teclas dentro del modo búsqueda:
 *   Letras/números  → añadir al término de búsqueda
 *   Backspace       → borrar último carácter del término
 *   ↓ / Ctrl+N      → siguiente ocurrencia
 *   ↑ / Ctrl+P      → ocurrencia anterior
 *   Enter           → confirmar y salir (cursor queda en el match)
 *   ESC             → cancelar y restaurar posición original
 */
static void editor_find_next(EditorState *e, const char *query, int dir) {
    if (!query[0] || e->nlines == 0) return;
    int qlen = (int)strlen(query);

    for (int i = 1; i <= e->nlines; i++) {
        int row = ((e->cy + dir * i) % e->nlines + e->nlines) % e->nlines;
        Line *l = &e->lines[row];
        char *found = NULL;

        if (dir > 0) {
            /* Adelante: buscar desde col 0 en filas siguientes */
            found = (l->len >= qlen) ? strstr(l->data, query) : NULL;
        } else {
            /* Atrás: última ocurrencia de la línea */
            for (int c = l->len - qlen; c >= 0; c--) {
                if (strncmp(l->data + c, query, qlen) == 0) {
                    found = l->data + c;
                    break;
                }
            }
        }

        if (found) {
            e->cy = row;
            e->cx = (int)(found - l->data);
            /* Centrar verticalmente si el match queda fuera de pantalla */
            if (e->cy < e->rowoff || e->cy >= e->rowoff + e->screenrows)
                e->rowoff = e->cy - e->screenrows / 2;
            if (e->rowoff < 0) e->rowoff = 0;
            return;
        }
    }
    snprintf(e->statusmsg, sizeof(e->statusmsg),
             "  '%s' no encontrado", query);
}

static void editor_find(EditorState *e) {
    char query[128] = {0};
    int  qlen = 0;
    int  dir  = 1;

    /* Guardar posición para restaurar si el usuario cancela */
    int saved_cx     = e->cx,     saved_cy  = e->cy;
    int saved_rowoff = e->rowoff, saved_coloff = e->coloff;

    while (1) {
        snprintf(e->statusmsg, sizeof(e->statusmsg),
                 "  Buscar: %s  [↓ siguiente  ↑ anterior  ESC cancelar]",
                 query);
        editor_draw(e);

        int key = read_key();
        if (key < 0) continue;   /* timeout de read(), ninguna tecla pulsada */

        if (key == '\033') {
            /* Cancelar: volver a la posición original */
            e->cx      = saved_cx;
            e->cy      = saved_cy;
            e->rowoff  = saved_rowoff;
            e->coloff  = saved_coloff;
            break;
        } else if (key == '\r' || key == '\n') {
            /* Confirmar: el cursor ya está en el match */
            break;
        } else if (key == KEY_BACKSP && qlen > 0) {
            query[--qlen] = '\0';
        } else if ((key == KEY_DOWN || key == CTRL_KEY('n')) && qlen > 0) {
            dir = 1;
            editor_find_next(e, query, dir);
        } else if ((key == KEY_UP || key == CTRL_KEY('p')) && qlen > 0) {
            dir = -1;
            editor_find_next(e, query, dir);
        } else if (!iscntrl(key) && key < 128) {
            if (qlen < (int)sizeof(query) - 1) {
                query[qlen++] = (char)key;
                query[qlen]   = '\0';
                dir = 1;
                /* Saltar automáticamente al primer match al escribir */
                editor_find_next(e, query, dir);
            }
        }
    }

    e->statusmsg[0] = '\0';
}

/* ── Procesamiento de teclas ─────────────────────────────*/
static void editor_process_key(EditorState *e, int key) {
    /* Limpiar mensaje de estado con la siguiente tecla */
    e->statusmsg[0] = '\0';

    switch (key) {

        case CTRL_KEY('q'):
            if (e->dirty && e->quit_confirm > 0) {
                snprintf(e->statusmsg, sizeof(e->statusmsg),
                         "  Hay cambios sin guardar. Ctrl+Q otra vez para salir sin guardar.");
                e->quit_confirm--;
            } else {
                e->should_quit = 1;
            }
            break;

        case CTRL_KEY('s'):
            editor_save(e);
            break;

        case CTRL_KEY('z'):
            editor_snap_pop(e);
            break;

        case CTRL_KEY('f'):
            editor_find(e);
            break;

        case KEY_UP:    e->cy--; editor_clamp_cursor(e); break;
        case KEY_DOWN:  e->cy++; editor_clamp_cursor(e); break;
        case KEY_LEFT:
            if (e->cx > 0) {
                e->cx--;
            } else if (e->cy > 0) {
                e->cy--;
                e->cx = e->lines[e->cy].len;
            }
            break;
        case KEY_RIGHT:
            if (e->cy < e->nlines) {
                if (e->cx < e->lines[e->cy].len) {
                    e->cx++;
                } else {
                    e->cy++; e->cx = 0;
                }
            }
            break;

        case KEY_HOME: e->cx = 0; break;
        case KEY_END:
            if (e->cy < e->nlines) e->cx = e->lines[e->cy].len;
            break;

        case KEY_PAGE_UP:
            e->cy -= e->screenrows;
            editor_clamp_cursor(e);
            break;
        case KEY_PAGE_DN:
            e->cy += e->screenrows;
            editor_clamp_cursor(e);
            break;

        case KEY_BACKSP:
            editor_snap_push(e);
            editor_delete_char(e);
            break;

        case KEY_DEL:
            editor_snap_push(e);
            editor_delete_forward(e);
            break;

        case '\r':
        case '\n':
            editor_snap_push(e);
            editor_insert_newline(e);
            break;

        case '\t':
            editor_snap_push(e);
            for (int i = 0; i < 4; i++) editor_insert_char(e, ' ');
            break;

        default:
            if (!iscntrl(key) && key < 128) {
                editor_snap_push(e);
                editor_insert_char(e, (char)key);
            }
            break;
    }

    editor_clamp_cursor(e);
}

/* ── Entrada pública ─────────────────────────────────────*/
int editor_open(vfs_t *vfs, const char *path) {
    EditorState e;
    memset(&e, 0, sizeof(e));
    e.vfs         = vfs;
    e.quit_confirm = 1;
    strncpy(e.filename, path, sizeof(e.filename) - 1);

    int total_rows, cols;
    get_window_size(&total_rows, &cols);
    e.screenrows = total_rows - 2;  /* -2: barra de estado + mensajes */
    e.screencols = cols;

    /* Cargar primero — si falla, no llegamos a tocar el terminal */
    if (editor_load(&e, path) < 0)
        return -1;

    if (enable_raw_mode() < 0) {
        fprintf(stderr, "editor: no se pudo activar modo raw\n");
        for (int i = 0; i < e.nlines; i++) free(e.lines[i].data);
        free(e.lines);
        return -1;
    }

    /* Limpiar pantalla al entrar */
    write(STDOUT_FILENO, "\033[2J\033[H", 7);

    while (!e.should_quit) {
        editor_draw(&e);
        int key = read_key();
        if (key >= 0) editor_process_key(&e, key);
    }

    /* Limpiar pantalla al salir y restaurar terminal */
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
    disable_raw_mode();

    /* Liberar memoria */
    for (int i = 0; i < e.nlines; i++) free(e.lines[i].data);
    free(e.lines);
    for (int i = 0; i < e.undo_count; i++) free(e.undo_stack[i].text);

    return 0;
}
