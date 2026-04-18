bits 32
global gdt_flush

; void gdt_flush(unsigned int gdt_ptr_addr)
;
; Carga la nueva GDT y recarga todos los registros de segmento.
; CS no se puede cargar directamente — requiere un far jump.
gdt_flush:
    mov eax, [esp+4]    ; puntero a struct gdt_ptr
    lgdt [eax]

    mov ax, 0x10        ; selector de datos (índice 2 × 8 = 0x10)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.reload_cs ; far jump recarga CS (índice 1 × 8 = 0x08)
.reload_cs:
    ret
