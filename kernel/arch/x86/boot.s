bits 32

; ── Multiboot 1 header ───────────────────────────────────
MULTIBOOT_MAGIC    equ 0x1BADB002
MULTIBOOT_FLAGS    equ 0
MULTIBOOT_CHECKSUM equ -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

section .multiboot
align 4
    dd MULTIBOOT_MAGIC
    dd MULTIBOOT_FLAGS
    dd MULTIBOOT_CHECKSUM

; ── Stack del kernel (16 KB) ─────────────────────────────
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; ── Punto de entrada ─────────────────────────────────────
; eax = 0x2BADB002  (magic multiboot)
; ebx = puntero a multiboot_info
section .text
global _start
extern kmain

_start:
    mov  esp, stack_top
    push ebx            ; arg2: multiboot_info*
    push eax            ; arg1: magic
    call kmain
.hang:
    cli
    hlt
    jmp .hang
