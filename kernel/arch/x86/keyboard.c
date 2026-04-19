/*
 * keyboard.c (x86) — Driver PS/2 para teclado, layout ES
 *
 * El teclado PS/2 envía scancodes (set 1) por el puerto 0x60.
 * Bit 7 = 1 → tecla soltada. Teclas extendidas van precedidas de 0xE0.
 */

#include "keyboard.h"
#include "idt.h"
#include "pic.h"

#define KBD_DATA 0x60

/* ── Tablas scancode → ASCII, layout ES (teclado español) ─────────────
 * Caracteres no-ASCII usan CP437 (página de código VGA):
 *   ñ=0xA4  Ñ=0xA5  ç=0x87  Ç=0x80 */

static const char sc_normal[128] = {
/*00*/  0,   27, '1','2','3','4','5','6','7','8','9','0','\'',  0,  '\b','\t',
/*10*/ 'q', 'w','e','r','t','y','u','i','o','p','`', '+', '\n',  0,  'a', 's',
/*20*/ 'd', 'f','g','h','j','k','l',(char)0xA4,  0,   0,   0,(char)0x87,'z','x','c','v',
/*30*/ 'b', 'n','m',',','.', '-',  0, '*',  0, ' ',  0,   0,   0,  0,  0,  0,
/*40*/   0,   0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
/*50*/ '2', '3','0','.',  0,  0, '<',  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*60*/   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*70*/   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

static const char sc_shift[128] = {
/*00*/  0,   27, '!','"',  0, '$','%','&','/','(',')','=', '?',  0,  '\b','\t',
/*10*/ 'Q', 'W','E','R','T','Y','U','I','O','P', '^','*', '\n',  0,  'A', 'S',
/*20*/ 'D', 'F','G','H','J','K','L',(char)0xA5,  0,   0,   0,(char)0x80,'Z','X','C','V',
/*30*/ 'B', 'N','M',';',':','_',   0, '*',  0, ' ',  0,   0,   0,  0,  0,  0,
/*40*/   0,   0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
/*50*/ '2', '3','0','.',  0,  0, '>',  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*60*/   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*70*/   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

/* ── Buffer circular ─────────────────────────────────────*/
#define BUF_SIZE 256
static unsigned char buf[BUF_SIZE];
static unsigned int  head = 0;
static unsigned int  tail = 0;

static int shift_down = 0;
static int e0_prefix  = 0;

static void buf_push(unsigned char c) {
    unsigned int next = (tail + 1) & (BUF_SIZE - 1);
    if (next != head) {
        buf[tail] = c;
        tail = next;
    }
}

/* ── Handler IRQ1 ────────────────────────────────────────*/

static void kbd_handler(struct registers *r) {
    (void)r;
    unsigned char sc = 0;
    __asm__ volatile("inb %1, %0" : "=a"(sc) : "Nd"((unsigned short)KBD_DATA));

    if (sc == 0xE0) { e0_prefix = 1; return; }

    if (sc & 0x80) {
        unsigned char key = sc & 0x7F;
        if (!e0_prefix && (key == 0x2A || key == 0x36)) shift_down = 0;
        e0_prefix = 0;
        return;
    }

    if (e0_prefix) { e0_prefix = 0; return; }  /* tecla extendida → ignorar */

    /* Tecla pulsada */
    if (sc == 0x2A || sc == 0x36) { shift_down = 1; return; }
    if (sc == 0x1D || sc == 0x38) return;
    if (sc == 0x3A) return;

    char c = shift_down ? sc_shift[sc] : sc_normal[sc];
    if (c) buf_push((unsigned char)c);
}

/* ── API pública ─────────────────────────────────────────*/

void keyboard_init(void) {
    irq_install_handler(1, kbd_handler);
    pic_unmask(1);
}

int keyboard_getchar(void) {
    if (head == tail) return -1;
    unsigned char c = buf[head];
    head = (head + 1) & (BUF_SIZE - 1);
    return (int)c;
}
