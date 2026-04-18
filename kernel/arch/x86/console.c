/*
 * console.c (x86) — Driver de texto VGA
 *
 * Implementa la interfaz console.h usando el buffer de texto VGA
 * en la dirección física 0xB8000.
 *
 * En RPi este archivo se sustituye por arch/arm/console.c
 * que escribe por el UART (PL011), sin cambiar nada más del kernel.
 *
 * Formato de celda VGA (2 bytes):
 *   byte bajo  = carácter ASCII
 *   byte alto  = [fondo 4 bits][color 4 bits]
 *   0x0A = texto verde brillante sobre fondo negro
 */

#include "../../console.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_COLOR  0x0A

static volatile unsigned short *const VGA_BUF =
    (volatile unsigned short *)0xB8000;

static int col = 0;
static int row = 0;

/* ── Cursor hardware VGA ────────────────────────────────
 * El cursor de parpadeo se mueve escribiendo la posición lineal
 * (row * VGA_WIDTH + col) en los registros CRTC 0x0E y 0x0F.
 * Se llama UNA VEZ al final de cada función de impresión (no por
 * carácter) para no saturar los puertos I/O con actualizaciones. */
#define VGA_CTRL 0x3D4
#define VGA_DATA 0x3D5

static void outb_con(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void cursor_update(void) {
    unsigned int pos = (unsigned int)(row * VGA_WIDTH + col);
    outb_con(VGA_CTRL, 0x0F);
    outb_con(VGA_DATA, (unsigned char)(pos & 0xFF));
    outb_con(VGA_CTRL, 0x0E);
    outb_con(VGA_DATA, (unsigned char)((pos >> 8) & 0xFF));
}

static void scroll(void) {
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
        VGA_BUF[i] = VGA_BUF[i + VGA_WIDTH];
    for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++)
        VGA_BUF[i] = (unsigned short)' ' | ((unsigned short)VGA_COLOR << 8);
    row = VGA_HEIGHT - 1;
}

static void putchar(char c) {
    if (c == '\n') {
        col = 0;
        if (++row >= VGA_HEIGHT) scroll();
        return;
    }
    if (c == '\b') {
        if (col > 0) col--;
        else if (row > 0) { row--; col = VGA_WIDTH - 1; }
        return;
    }
    VGA_BUF[row * VGA_WIDTH + col] =
        (unsigned short)(unsigned char)c | ((unsigned short)VGA_COLOR << 8);
    if (++col >= VGA_WIDTH) {
        col = 0;
        if (++row >= VGA_HEIGHT) scroll();
    }
}

void console_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_BUF[i] = (unsigned short)' ' | ((unsigned short)VGA_COLOR << 8);
    col = 0;
    row = 0;
    cursor_update();
}

void console_print(const char *s) {
    while (*s) putchar(*s++);
    cursor_update();
}

void console_print_hex(unsigned int n) {
    console_print("0x");
    for (int i = 28; i >= 0; i -= 4) {
        unsigned char d = (n >> i) & 0xF;
        putchar(d < 10 ? '0' + d : 'A' + d - 10);
    }
    cursor_update();
}

void console_print_uint(unsigned int n) {
    if (n == 0) { putchar('0'); return; }
    char buf[12];
    int i = 0;
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) putchar(buf[i]);
    cursor_update();
}

void console_print_buf(const char *buf, unsigned int len) {
    for (unsigned int i = 0; i < len; i++) putchar(buf[i]);
    cursor_update();
}
