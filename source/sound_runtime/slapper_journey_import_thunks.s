/*
 * Bridge from the sound runtime to functions already imported by the
 * supported Journey PC executable.
 *
 * The target image has no relocation directory and loads at 0x140000000.
 * The patcher verifies every IAT slot by its localized PE
 * import identity before admitting an input executable.
 */

        .text

        .macro iat_thunk name, address
        .p2align 4
        .globl \name
        .def \name; .scl 2; .type 32; .endef
\name:
        movabsq $\address, %r11
        jmpq *(%r11)
        .endm

        iat_thunk cos,    0x140553a90
        iat_thunk exp,    0x140553a98
        iat_thunk pow,    0x140553ad8
        iat_thunk sin,    0x140553ae8
        iat_thunk malloc, 0x1405539d0
        iat_thunk free,   0x1405539e8
        iat_thunk memset, 0x1405537a0
        iat_thunk memcpy, 0x1405537c8
        iat_thunk GetModuleHandleA, 0x1405530b8
        iat_thunk GetProcAddress,    0x140553098

        /* The recovered path clamps both discriminants non-negative first. */
        .p2align 4
        .globl sqrt
        .def sqrt; .scl 2; .type 32; .endef
sqrt:
        sqrtsd %xmm0, %xmm0
        ret
