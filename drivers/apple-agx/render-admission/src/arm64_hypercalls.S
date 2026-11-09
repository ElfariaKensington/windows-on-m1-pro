// GNU-assembler twin of arm64_hypercalls.asm for clang builds.
        .text
        .p2align 2
        .globl AdmissionHvcArmConsumed
AdmissionHvcArmConsumed:
        hvc #0x4d32
        ret
        .globl AdmissionHvcGuestIpaPa
AdmissionHvcGuestIpaPa:
        hvc #0x4d31
        ret
