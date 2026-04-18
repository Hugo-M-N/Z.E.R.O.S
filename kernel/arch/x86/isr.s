bits 32

; ── Macros ───────────────────────────────────────────────
; Excepciones SIN código de error: empujamos 0 como código falso
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

; Excepciones CON código de error: la CPU ya lo empujó
%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

; IRQs hardware
%macro IRQ_STUB 2
global irq%1
irq%1:
    push dword 0
    push dword %2
    jmp irq_common
%endmacro

; ── Excepciones CPU 0-31 ─────────────────────────────────
ISR_NOERR  0   ; divide by zero
ISR_NOERR  1   ; debug
ISR_NOERR  2   ; NMI
ISR_NOERR  3   ; breakpoint
ISR_NOERR  4   ; overflow
ISR_NOERR  5   ; bound range exceeded
ISR_NOERR  6   ; invalid opcode
ISR_NOERR  7   ; device not available
ISR_ERR    8   ; double fault
ISR_NOERR  9   ; coprocessor segment overrun
ISR_ERR   10   ; invalid TSS
ISR_ERR   11   ; segment not present
ISR_ERR   12   ; stack-segment fault
ISR_ERR   13   ; general protection fault
ISR_ERR   14   ; page fault
ISR_NOERR 15
ISR_NOERR 16   ; x87 FPU error
ISR_ERR   17   ; alignment check
ISR_NOERR 18   ; machine check
ISR_NOERR 19   ; SIMD FP exception
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ── IRQs hardware (vectores 32-47) ───────────────────────
IRQ_STUB  0, 32   ; timer PIT
IRQ_STUB  1, 33   ; teclado
IRQ_STUB  2, 34
IRQ_STUB  3, 35
IRQ_STUB  4, 36
IRQ_STUB  5, 37
IRQ_STUB  6, 38
IRQ_STUB  7, 39
IRQ_STUB  8, 40
IRQ_STUB  9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

; ── Manejador común de excepciones ───────────────────────
; Stack al llegar aquí (arriba = menor dirección):
;   int_no, err_code   ← empujados por el stub
;   eip, cs, eflags    ← empujados por la CPU
extern isr_handler

isr_common:
    pusha               ; edi, esi, ebp, esp, ebx, edx, ecx, eax
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10        ; segmento de datos del kernel
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp            ; puntero a struct registers
    call isr_handler
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; limpiar int_no + err_code
    iret

; ── Manejador común de IRQs ──────────────────────────────
; irq_handler devuelve en eax el nuevo esp si hay cambio de proceso,
; o 0 si no hay cambio. Si hay cambio, cargamos el nuevo esp antes
; de restaurar el contexto — así el iret final entra en el nuevo proceso.
extern irq_handler

irq_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp
    call irq_handler    ; devuelve nuevo esp en eax (0 = sin cambio)
    add esp, 4
    test eax, eax
    jz .no_switch
    mov esp, eax        ; cambiar al stack del siguiente proceso
.no_switch:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret
