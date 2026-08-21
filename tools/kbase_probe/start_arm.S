.section .text
.global _start
.type _start, %function
_start:
    mov r0, sp
    bl main
    mov r7, #248
    svc #0
.size _start, . - _start
