.section .text
.global _start
_start:
    xor %ebp, %ebp
    mov (%esp), %eax          /* argc */
    lea 4(%esp), %ecx         /* argv */
    push %ecx
    push %eax
    call main
    mov %eax, %ebx            /* exit code = main's return value */
    mov $1, %eax              /* sys_exit */
    int $0x80
1:  jmp 1b

.section .note.GNU-stack,"",@progbits