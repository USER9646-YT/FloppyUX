.section .text
.global switch_context
.type switch_context, @function
/* void switch_context(uint32_t *old_ksp, uint32_t new_ksp) */
switch_context:
    mov 4(%esp), %eax        /* where to save the old stack pointer */
    mov 8(%esp), %edx        /* new stack pointer                   */
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov %esp, (%eax)
    mov %edx, %esp
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp
    ret