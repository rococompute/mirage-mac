// Granule spike guest. Runs with the MMU off, so VA == IPA.
// Each entry point ends in HVC #0 on success; the host checks the exit.
    .text
    .global _start
_start:
    .org 0x000                  // (a) store to the R page: expect stage-2 DABT
    mov x0, #0x1000
    str w1, [x0]
    hvc #0
    .org 0x040                  // (b) store to the RW page: expect HVC
    mov x0, #0x2000
    str w1, [x0]
    hvc #0
    .org 0x080                  // (c) branch into the RW (no X) page: expect IABT
    mov x0, #0x2000
    br x0
    .org 0x0c0                  // (d) branch into the X-only page: expect HVC
    mov x0, #0x3000
    br x0
    .org 0x100                  // (e) load from the X-only page: DABT if XO works
    mov x0, #0x3000
    ldr w1, [x0]
    hvc #0
    .org 0x140                  // (f) load from the R page: expect HVC
    mov x0, #0x1000
    ldr w1, [x0]
    hvc #0
    .org 0x180                  // (g) perf: x0=base, x2=bytes, x3=passes
1:  mov x4, x0
    mov x5, x2
2:  str x1, [x4]
    add x4, x4, #4096
    subs x5, x5, #4096
    b.ne 2b
    subs x3, x3, #1
    b.ne 1b
    hvc #0
    .org 0x2000
    hvc #0
    .org 0x3000
    hvc #0
    .org 0x4000
