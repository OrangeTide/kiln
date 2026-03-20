; greeting.asm : x86-64 Windows (PE/COFF) assembly
; Returns a pointer to a greeting string.
;
; Assemble with: nasm -f win64 greeting.asm -o greeting.o

bits 64
default rel

section .data
    msg db "Hello from x86-64 assembly on Windows!", 0

section .text
    global get_greeting

; const char *get_greeting(void)
; Windows x64 ABI: return value in rax
get_greeting:
    lea rax, [msg]
    ret
