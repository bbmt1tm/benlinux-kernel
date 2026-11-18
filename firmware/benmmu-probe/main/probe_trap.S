/*
 * BenMMU probe — Core 1 trap vector, user-mode runner and MPRV helpers.
 *
 * Runs only inside probe_env_enter()/probe_env_exit(): Core 1 has MIE=0,
 * mtvec points at probe_trap_vector and mscratch holds the top of a
 * dedicated trap stack. Only synchronous exceptions arrive here.
 *
 * Trap frame (144 bytes, matches trap_frame_t in probe.h):
 *   0..124   x0..x31 (x0 slot unused, x2 = interrupted sp)
 *   128      mepc
 *   132      mcause
 *   136      mtval
 *   140      mstatus
 */

    .equ FRAME,      144
    .equ OFF_MEPC,   128
    .equ OFF_MCAUSE, 132
    .equ OFF_MTVAL,  136
    .equ OFF_MSTAT,  140

    .equ MSTATUS_MPP,  0x1800
    .equ MSTATUS_MPIE, 0x80
    .equ MSTATUS_MPRV, 0x20000

/* ------------------------------------------------------------------ */
/* Trap vector. CLIC mode: exceptions go to mtvec[31:6], so 64-byte    */
/* alignment is required.                                              */
/* ------------------------------------------------------------------ */
    .section .iram1.probe_trap, "ax"
    .balign 64
    .global probe_trap_vector
    .type   probe_trap_vector, @function
probe_trap_vector:
    csrrw   sp, mscratch, sp            /* sp = trap stack, mscratch = old sp */
    addi    sp, sp, -FRAME
    sw      x1,   4(sp)
    sw      x3,  12(sp)
    sw      x4,  16(sp)
    sw      x5,  20(sp)
    sw      x6,  24(sp)
    sw      x7,  28(sp)
    sw      x8,  32(sp)
    sw      x9,  36(sp)
    sw      x10, 40(sp)
    sw      x11, 44(sp)
    sw      x12, 48(sp)
    sw      x13, 52(sp)
    sw      x14, 56(sp)
    sw      x15, 60(sp)
    sw      x16, 64(sp)
    sw      x17, 68(sp)
    sw      x18, 72(sp)
    sw      x19, 76(sp)
    sw      x20, 80(sp)
    sw      x21, 84(sp)
    sw      x22, 88(sp)
    sw      x23, 92(sp)
    sw      x24, 96(sp)
    sw      x25, 100(sp)
    sw      x26, 104(sp)
    sw      x27, 108(sp)
    sw      x28, 112(sp)
    sw      x29, 116(sp)
    sw      x30, 120(sp)
    sw      x31, 124(sp)
    csrr    t0, mscratch
    sw      t0,   8(sp)                 /* interrupted sp */
    csrr    t0, mepc
    sw      t0, OFF_MEPC(sp)
    csrr    t0, mcause
    sw      t0, OFF_MCAUSE(sp)
    csrr    t0, mtval
    sw      t0, OFF_MTVAL(sp)
    csrr    t0, mstatus
    sw      t0, OFF_MSTAT(sp)

    mv      a0, sp
    call    probe_trap_c                /* a0: 0 = resume, 1 = leave user mode */
    bnez    a0, .Lleave_user

    lw      t0, OFF_MEPC(sp)
    csrw    mepc, t0
    lw      t0, OFF_MSTAT(sp)
    csrw    mstatus, t0
    lw      x1,   4(sp)
    lw      x3,  12(sp)
    lw      x4,  16(sp)
    lw      x5,  20(sp)
    lw      x6,  24(sp)
    lw      x7,  28(sp)
    lw      x8,  32(sp)
    lw      x9,  36(sp)
    lw      x10, 40(sp)
    lw      x11, 44(sp)
    lw      x12, 48(sp)
    lw      x13, 52(sp)
    lw      x14, 56(sp)
    lw      x15, 60(sp)
    lw      x16, 64(sp)
    lw      x17, 68(sp)
    lw      x18, 72(sp)
    lw      x19, 76(sp)
    lw      x20, 80(sp)
    lw      x21, 84(sp)
    lw      x22, 88(sp)
    lw      x23, 92(sp)
    lw      x24, 96(sp)
    lw      x25, 100(sp)
    lw      x26, 104(sp)
    lw      x27, 108(sp)
    lw      x28, 112(sp)
    lw      x29, 116(sp)
    lw      x30, 120(sp)
    lw      x31, 124(sp)
    addi    sp, sp, FRAME
    csrrw   sp, mscratch, sp            /* sp = old sp, mscratch = trap stack */
    mret

.Lleave_user:
    /* Abandon the user context and resume probe_run_user's caller in M-mode. */
    addi    sp, sp, FRAME
    csrw    mscratch, sp                /* trap stack top back into mscratch */
    j       probe_user_resume
    .size   probe_trap_vector, . - probe_trap_vector

/* ------------------------------------------------------------------ */
/* void probe_run_user(uint32_t fn, uint32_t user_sp, uint32_t arg)    */
/* Runs fn(arg) in U-mode on user_sp. fn returns into                  */
/* probe_user_exit_stub, whose ecall brings us back via .Lleave_user.  */
/* ------------------------------------------------------------------ */
    .section .iram1.probe_user, "ax"
    .balign 4
    .global probe_run_user
    .type   probe_run_user, @function
probe_run_user:
    addi    sp, sp, -64
    sw      ra,   0(sp)
    sw      s0,   4(sp)
    sw      s1,   8(sp)
    sw      s2,  12(sp)
    sw      s3,  16(sp)
    sw      s4,  20(sp)
    sw      s5,  24(sp)
    sw      s6,  28(sp)
    sw      s7,  32(sp)
    sw      s8,  36(sp)
    sw      s9,  40(sp)
    sw      s10, 44(sp)
    sw      s11, 48(sp)
    la      t0, probe_saved_msp
    sw      sp, 0(t0)

    csrw    mepc, a0
    li      t0, MSTATUS_MPP
    csrc    mstatus, t0                 /* MPP = U */
    li      t0, MSTATUS_MPIE
    csrc    mstatus, t0                 /* interrupts stay off in U */
    li      t0, MSTATUS_MPRV
    csrc    mstatus, t0
    mv      sp, a1
    mv      a0, a2
    la      ra, probe_user_exit_stub
    mret

    .global probe_user_resume
probe_user_resume:
    la      t0, probe_saved_msp
    lw      sp, 0(t0)
    li      t0, MSTATUS_MPP
    csrs    mstatus, t0                 /* MPP = M again */
    lw      ra,   0(sp)
    lw      s0,   4(sp)
    lw      s1,   8(sp)
    lw      s2,  12(sp)
    lw      s3,  16(sp)
    lw      s4,  20(sp)
    lw      s5,  24(sp)
    lw      s6,  28(sp)
    lw      s7,  32(sp)
    lw      s8,  36(sp)
    lw      s9,  40(sp)
    lw      s10, 44(sp)
    lw      s11, 48(sp)
    addi    sp, sp, 64
    ret
    .size   probe_run_user, . - probe_run_user

    .balign 4
    .global probe_user_exit_stub
    .type   probe_user_exit_stub, @function
probe_user_exit_stub:
    li      a7, 0
    ecall
1:  j       1b
    .size   probe_user_exit_stub, . - probe_user_exit_stub

/* ------------------------------------------------------------------ */
/* MPRV helpers (P3). M-mode load/store with mstatus.MPRV=1 and a       */
/* chosen MPP, so the access is PMP-checked at that privilege.          */
/* uint32_t probe_mprv_load(uint32_t addr, uint32_t mpp)                */
/* void     probe_mprv_store(uint32_t addr, uint32_t mpp, uint32_t val) */
/* If the access faults the handler skips it; a0 then still holds addr. */
/* ------------------------------------------------------------------ */
    .balign 4
    .global probe_mprv_load
    .type   probe_mprv_load, @function
probe_mprv_load:
    li      t1, MSTATUS_MPP
    csrc    mstatus, t1
    slli    t2, a1, 11
    csrs    mstatus, t2
    li      t1, MSTATUS_MPRV
    csrs    mstatus, t1
    .global probe_mprv_load_pc
probe_mprv_load_pc:
    lw      a0, 0(a0)
    li      t1, MSTATUS_MPRV
    csrc    mstatus, t1
    li      t1, MSTATUS_MPP
    csrs    mstatus, t1
    ret
    .size   probe_mprv_load, . - probe_mprv_load

    .balign 4
    .global probe_mprv_store
    .type   probe_mprv_store, @function
probe_mprv_store:
    li      t1, MSTATUS_MPP
    csrc    mstatus, t1
    slli    t2, a1, 11
    csrs    mstatus, t2
    li      t1, MSTATUS_MPRV
    csrs    mstatus, t1
    .global probe_mprv_store_pc
probe_mprv_store_pc:
    sw      a2, 0(a0)
    li      t1, MSTATUS_MPRV
    csrc    mstatus, t1
    li      t1, MSTATUS_MPP
    csrs    mstatus, t1
    ret
    .size   probe_mprv_store, . - probe_mprv_store
