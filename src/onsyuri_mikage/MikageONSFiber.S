// Stack switching for running ONScripter's blocking loop on the main thread.
// arm64 Apple ABI: x19-x28, fp, lr and the low halves of v8-v15 are
// callee-saved, so they are all that has to survive a switch.

#if defined(__aarch64__) && defined(__APPLE__)

.text

// void mikage_ons_fiber_switch(void **save_sp, void *load_sp)
// Saves the current context on its stack, stores that stack pointer in
// *save_sp, then resumes the context saved at load_sp.
.globl _mikage_ons_fiber_switch
.private_extern _mikage_ons_fiber_switch
.p2align 2
_mikage_ons_fiber_switch:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x9, sp
    str x9, [x0]

    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret

// First code a new fiber runs: mikage_ons_fiber_prepare puts the entry
// function in x19 and its argument in x20. The entry never returns; it
// switches away for good when the game ends.
.globl _mikage_ons_fiber_trampoline
.private_extern _mikage_ons_fiber_trampoline
.p2align 2
_mikage_ons_fiber_trampoline:
    mov x0, x20
    blr x19
    brk #0xf1

#endif
