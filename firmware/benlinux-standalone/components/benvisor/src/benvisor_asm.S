/*
 * benvisor_asm.S — BenVisor Assembly for ESP32-P4 Core 1 (Standalone)
 *
 * Timer ISR:       Systimer Alarm 1 → IPC console drain + heartbeat
 * Unexpected ISR:  Default handler for unconfigured mtvt slots
 * Boot trampoline: Saves C context, jumps to kernel, returns on shutdown
 *
 * CONSTRAINTS:
 *   - mscratch is OFF LIMITS (Linux uses for tp/zero swap on every trap)
 *   - Save registers to Linux kernel stack (sp always valid, NOMMU M-mode)
 *   - CLIC mret restores MIE from mcause.MPIE (bit 27)
 *
 * NOTE: With SHV=0 for CLIC24, the systimer interrupt goes directly to
 * the kernel's mtvec handler — NOT through this ISR. This ISR is kept
 * in the mtvt table for: (a) IPC console drain via heartbeat, and
 * (b) future use with other CLIC entries.
 */

    .option norvc               /* No compressed instructions */

/* ========================================
 * Data Section — BenVisor State
 * ======================================== */

    .section .bss
    .balign 4

    .global benvisor_mtimecmp_ptr
benvisor_mtimecmp_ptr:
    .word   0

    .global benvisor_ipc_ptr
benvisor_ipc_ptr:
    .word   0

    .global benvisor_heartbeat_ctr
benvisor_heartbeat_ctr:
    .word   0

    .global benvisor_isr_count
benvisor_isr_count:
    .word   0

    .global benvisor_last_mepc
benvisor_last_mepc:
    .word   0

    .global benvisor_diag_mtvec
benvisor_diag_mtvec:
    .word   0

    .global benvisor_diag_mstatus
benvisor_diag_mstatus:
    .word   0

    .global benvisor_saved_mcause
benvisor_saved_mcause:
    .word   0

    /* Bail breadcrumbs: a tick that fires while sp==0 takes the bail
     * path. We count those and record the PC running with sp==0 so
     * Core 0 can see them and identify the offending context. */
    .global benvisor_bail_count
benvisor_bail_count:
    .word   0

    .global benvisor_bail_mepc
benvisor_bail_mepc:
    .word   0

    /* Private stack for the bail path. When sp==0 we cannot push to the
     * kernel stack and mscratch is off limits, so we borrow this to get
     * scratch registers, clear the alarm, and re-arm. 512 bytes is ample
     * (the bail does not nest: it runs at CLIC level 7, the max). */
    .balign 16
benvisor_isr_bail_stack:
    .space  512
benvisor_isr_bail_stack_top:

    .global benvisor_saved_mtvec
benvisor_saved_mtvec:
    .word   0

    .global benvisor_saved_mtvt
benvisor_saved_mtvt:
    .word   0

    /* Boot trampoline: saved callee-saved registers */
    .balign 4
benvisor_saved_regs:
    .space  14 * 4              /* s0-s11 + ra + sp = 14 regs */


/* ========================================
 * mtvt Vector Table for Core 1
 *
 * 48 entries. Each is a function pointer address.
 * Entry 24 = systimer Alarm 1 → benvisor_timer_isr
 * All others → benvisor_unexpected_isr
 *
 * MUST be in IRAM (.iram1) — CLIC reads via instruction bus.
 * Must be 64-byte aligned.
 * ======================================== */

    .section .iram1, "ax"
    .balign 64
    .global benvisor_mtvt_table
benvisor_mtvt_table:
    .rept 7
    .word   benvisor_unexpected_isr
    .endr
    /* Entry 7: PoC HARDCODE — kernel _start_trap for the SD-card Image only
     * (md5 873c397aa1fdcd1eb139cef15d10267e, _start_trap=0x484c78dc, verified).
     * Breaks on any future kernel rebuild that shifts entry.S above _start_trap;
     * replace with a DTB/IPC-carried address for the durable fix. */
    .word   0x484c78dc                  /* Entry 7 → kernel _start_trap */
    .rept 16
    .word   benvisor_unexpected_isr
    .endr
    .word   benvisor_timer_isr          /* Entry 24 */
    .rept 23
    .word   benvisor_unexpected_isr
    .endr


/* ========================================
 * Timer ISR — Systimer Alarm 1 Handler
 *
 * Called via mtvt[24] when SHV=1 (or when the
 * kernel's mtvec routes here). Handles IPC
 * console drain and heartbeat.
 *
 * Register usage: saves ALL caller-saved regs
 * to kernel stack. Never touches mscratch.
 * ======================================== */

    .section .iram1, "ax"
    .balign 4
    .global benvisor_timer_isr
    .type   benvisor_timer_isr, @function

benvisor_timer_isr:
    /* Guard: if sp==0 the kernel context has no usable stack (e.g. first
     * tick after mret into a NOMMU userspace whose sp is still 0). The old
     * bail did a bare mret WITHOUT clearing INT_ST — but line 24 is
     * level-triggered, so it re-fired instantly → infinite storm, hard hang.
     * The bail path now clears the alarm + re-arms on a private stack. */
    beqz    sp, .Lisr_bail_fast

    /* Save ALL caller-saved registers (16 regs = 64 bytes) */
    addi    sp, sp, -64
    sw      ra,  0(sp)
    sw      t0,  4(sp)
    sw      t1,  8(sp)
    sw      t2, 12(sp)
    sw      t3, 16(sp)
    sw      t4, 20(sp)
    sw      t5, 24(sp)
    sw      t6, 28(sp)
    sw      a0, 32(sp)
    sw      a1, 36(sp)
    sw      a2, 40(sp)
    sw      a3, 44(sp)
    sw      a4, 48(sp)
    sw      a5, 52(sp)
    sw      a6, 56(sp)
    sw      a7, 60(sp)

    /* Save mcause: CLIC mret reads MPIL from here */
    la      t0, benvisor_saved_mcause
    csrr    t1, mcause
    sw      t1, 0(t0)

    /* ---- Clear systimer Alarm 1 interrupt ---- */
    lui     t0, %hi(0x500E206C)         /* SYSTIMER_INT_CLR */
    li      t1, 2                       /* bit 1 = Alarm 1 */
    sw      t1, %lo(0x500E206C)(t0)
    la      t0, benvisor_isr_count
    lw      t1, 0(t0)
    addi    t1, t1, 1
    sw      t1, 0(t0)

    /* ---- Breadcrumb: save kernel PC ---- */
    la      t0, benvisor_last_mepc
    csrr    t1, mepc
    sw      t1, 0(t0)

    /* ---- Diagnostic: save mtvec + mstatus ---- */
    la      t0, benvisor_diag_mtvec
    csrr    t1, mtvec
    sw      t1, 0(t0)

    la      t0, benvisor_diag_mstatus
    csrr    t1, mstatus
    sw      t1, 0(t0)

    /* ---- Tick injection: inject CLIC line 7 when the systimer counter has
     * reached the kernel's deadline (ST_TARGET1) ----
     *
     * The kernel timer driver writes its next deadline straight to the
     * systimer TARGET1 registers (0x500E2024/28) and never updates the IPC
     * mtimecmp (which is stuck at 0xFFFF...). It expects the alarm to deliver
     * to its hwirq handler. So BenVisor compares the live 16 MHz systimer
     * counter against TARGET1 — the SAME clock domain (NOT CSR time, which is
     * 360 MHz) — and injects line 7 (now vectored → mtvt[7] → kernel) when due.
     *
     * Registers (16 MHz domain):
     *   ST_UNIT0_OP   0x500E2004  write (1<<30) to latch counter
     *   ST_UNIT0_VAL  0x500E2040(hi) 0x500E2044(lo)
     *   ST_TARGET1    0x500E2024(hi) 0x500E2028(lo)
     * Uses t0..t4 (all saved in the frame, restored before mret). */

    /* latch current systimer counter */
    lui     t0, %hi(0x500E2004)
    lui     t1, 0x40000                 /* UNIT0_UPDATE = bit 30 */
    sw      t1, %lo(0x500E2004)(t0)

    /* read counter (hi:lo) */
    lui     t0, %hi(0x500E2040)
    lw      t3, %lo(0x500E2040)(t0)     /* t3 = now_hi */
    lui     t0, %hi(0x500E2044)
    lw      t4, %lo(0x500E2044)(t0)     /* t4 = now_lo */

    /* read kernel deadline TARGET1 (hi:lo) */
    lui     t0, %hi(0x500E2024)
    lw      t1, %lo(0x500E2024)(t0)     /* t1 = tgt_hi */
    lui     t0, %hi(0x500E2028)
    lw      t2, %lo(0x500E2028)(t0)     /* t2 = tgt_lo */

    /* 64-bit compare: due if now >= target */
    bltu    t3, t1, .Ltimer_done        /* now_hi < tgt_hi → not due */
    bne     t3, t1, .Ltimer_inject      /* now_hi > tgt_hi → due */
    bltu    t4, t2, .Ltimer_done        /* now_lo < tgt_lo → not due */

.Ltimer_inject:
    lui     t0, %hi(0x2080101C)         /* CLIC int 7 ctrl reg */
    lb      t1, %lo(0x2080101C)(t0)
    bnez    t1, .Ltimer_done            /* already pending → skip */
    li      t1, 1
    sb      t1, %lo(0x2080101C)(t0)     /* IP = 1 */

.Ltimer_done:
    /* ---- IPC Console Drain: 1 char per tick ---- */
    la      t0, benvisor_ipc_ptr
    lw      t0, 0(t0)
    beqz    t0, .Lskip_drain

    lw      t1, 0(t0)                  /* tx_head */
    lw      t2, 4(t0)                  /* tx_tail */
    beq     t1, t2, .Lskip_drain       /* empty */

    addi    t1, t0, 8                  /* &tx_buf[0] */
    add     t1, t1, t2                 /* &tx_buf[tx_tail] */
    lbu     a0, 0(t1)

    addi    t2, t2, 1
    andi    t2, t2, 0x7FF
    sw      t2, 4(t0)                  /* update tx_tail */

    call    esp_rom_output_tx_one_char

.Lskip_drain:
    /* ---- Heartbeat: '+' every ~1 second ---- */
    la      t0, benvisor_heartbeat_ctr
    lw      t1, 0(t0)
    addi    t1, t1, -1
    sw      t1, 0(t0)
    bnez    t1, .Lheartbeat_done

    li      t1, 10000                   /* 1 per second at 10kHz */
    sw      t1, 0(t0)
    li      a0, '+'
    call    esp_rom_output_tx_one_char

.Lheartbeat_done:
    /* ---- Re-arm alarm (MUST be last before restore) ---- */

    /* Disable TARGET1_EN (clear bit 23) */
    lui     t0, %hi(0x500E2000)
    lw      t1, %lo(0x500E2000)(t0)
    lui     t2, 0x00800
    not     t2, t2
    and     t1, t1, t2
    sw      t1, %lo(0x500E2000)(t0)

    /* Read current counter */
    lui     t0, %hi(0x500E2004)
    lui     t1, 0x40000                 /* bit 30 = UNIT0_UPDATE */
    sw      t1, %lo(0x500E2004)(t0)
    lui     t0, %hi(0x500E2044)
    lw      t1, %lo(0x500E2044)(t0)

    /* Set target = now + 1600 (100µs at 16MHz) */
    addi    t1, t1, 1600
    lui     t0, %hi(0x500E2028)
    sw      t1, %lo(0x500E2028)(t0)

    /* Load comparator */
    lui     t0, %hi(0x500E2054)
    li      t1, 1
    sw      t1, %lo(0x500E2054)(t0)

    /* Re-enable TARGET1_EN */
    lui     t0, %hi(0x500E2000)
    lw      t1, %lo(0x500E2000)(t0)
    lui     t2, 0x00800
    or      t1, t1, t2
    sw      t1, %lo(0x500E2000)(t0)

    /* Restore mcause */
    la      t0, benvisor_saved_mcause
    lw      t1, 0(t0)
    csrw    mcause, t1

    /* Restore ALL caller-saved registers */
    lw      ra,  0(sp)
    lw      t0,  4(sp)
    lw      t1,  8(sp)
    lw      t2, 12(sp)
    lw      t3, 16(sp)
    lw      t4, 20(sp)
    lw      t5, 24(sp)
    lw      t6, 28(sp)
    lw      a0, 32(sp)
    lw      a1, 36(sp)
    lw      a2, 40(sp)
    lw      a3, 44(sp)
    lw      a4, 48(sp)
    lw      a5, 52(sp)
    lw      a6, 56(sp)
    lw      a7, 60(sp)
    addi    sp, sp, 64
    mret

.Lisr_bail_fast:
    /* sp == 0 here, so there is nothing on the kernel stack to preserve.
     * Borrow the private bail stack to get three scratch registers, clear
     * the alarm so it stops re-firing, re-arm it at the normal cadence, and
     * leave a breadcrumb. Restore sp = 0 (we know it was 0) before mret. */
    la      sp, benvisor_isr_bail_stack_top
    addi    sp, sp, -16
    sw      t0, 0(sp)
    sw      t1, 4(sp)
    sw      t2, 8(sp)

    /* Clear systimer Alarm 1 INT_ST — this is what stops the storm. */
    lui     t0, %hi(0x500E206C)            /* SYSTIMER_INT_CLR */
    li      t1, 2                          /* bit 1 = Alarm 1 */
    sw      t1, %lo(0x500E206C)(t0)

    /* Breadcrumb: count bails + record the PC that was running with sp==0. */
    la      t0, benvisor_bail_count
    lw      t1, 0(t0)
    addi    t1, t1, 1
    sw      t1, 0(t0)
    la      t0, benvisor_bail_mepc
    csrr    t1, mepc
    sw      t1, 0(t0)

    /* Re-arm Alarm 1 = now + 1600 (same cadence as the normal path) so the
     * heartbeat keeps coming even while the offending context persists. */
    lui     t0, %hi(0x500E2000)            /* disable TARGET1_EN (bit 23) */
    lw      t1, %lo(0x500E2000)(t0)
    lui     t2, 0x00800
    not     t2, t2
    and     t1, t1, t2
    sw      t1, %lo(0x500E2000)(t0)

    lui     t0, %hi(0x500E2004)            /* latch current counter */
    lui     t1, 0x40000                    /* bit 30 = UNIT0_UPDATE */
    sw      t1, %lo(0x500E2004)(t0)
    lui     t0, %hi(0x500E2044)
    lw      t1, %lo(0x500E2044)(t0)

    addi    t1, t1, 1600                   /* target = now + 1600 */
    lui     t0, %hi(0x500E2028)
    sw      t1, %lo(0x500E2028)(t0)

    lui     t0, %hi(0x500E2054)            /* COMP1_LOAD */
    li      t1, 1
    sw      t1, %lo(0x500E2054)(t0)

    lui     t0, %hi(0x500E2000)            /* re-enable TARGET1_EN (bit 23) */
    lw      t1, %lo(0x500E2000)(t0)
    lui     t2, 0x00800
    or      t1, t1, t2
    sw      t1, %lo(0x500E2000)(t0)

    lw      t0, 0(sp)
    lw      t1, 4(sp)
    lw      t2, 8(sp)
    li      sp, 0                          /* restore sp == 0 (known) */
    mret

    .size benvisor_timer_isr, . - benvisor_timer_isr


/* ========================================
 * Unexpected Interrupt Handler
 * ======================================== */

    .section .iram1, "ax"
    .balign 4
    .global benvisor_unexpected_isr
    .type   benvisor_unexpected_isr, @function

benvisor_unexpected_isr:
    mret
    .size benvisor_unexpected_isr, . - benvisor_unexpected_isr


/* ========================================
 * Boot Trampoline
 *
 * Called from C: benvisor_boot_linux(entry, dtb, hartid)
 * Arguments: a0 = kernel entry, a1 = dtb address, a2 = hart ID
 *
 * Saves callee-saved regs, rearranges args per Linux boot protocol
 * (a0=hart_id, a1=dtb_addr), jumps to kernel.
 * ======================================== */

    .section .iram1, "ax"
    .balign 4
    .global benvisor_boot_linux
    .type   benvisor_boot_linux, @function

benvisor_boot_linux:
    /* Save all callee-saved registers */
    la      t0, benvisor_saved_regs
    sw      ra,  0(t0)
    sw      sp,  4(t0)
    sw      s0,  8(t0)
    sw      s1, 12(t0)
    sw      s2, 16(t0)
    sw      s3, 20(t0)
    sw      s4, 24(t0)
    sw      s5, 28(t0)
    sw      s6, 32(t0)
    sw      s7, 36(t0)
    sw      s8, 40(t0)
    sw      s9, 44(t0)
    sw      s10, 48(t0)
    sw      s11, 52(t0)

    /* Rearrange for Linux boot protocol:
     *   a0 = hart_id (was a2)
     *   a1 = dtb_addr (already a1) */
    mv      t0, a0             /* t0 = entry address */
    mv      a0, a2             /* a0 = hart_id */
    /* a1 already = dtb_addr */

    jr      t0                 /* Jump to kernel — never returns */

    .size benvisor_boot_linux, . - benvisor_boot_linux


/* ========================================
 * Return from Linux
 *
 * a0 = return value (0 = clean shutdown, 1 = forced)
 * ======================================== */

    .section .iram1, "ax"
    .balign 4
    .global benvisor_return_from_linux
    .type   benvisor_return_from_linux, @function

benvisor_return_from_linux:
    la      t0, benvisor_saved_regs
    lw      ra,  0(t0)
    lw      sp,  4(t0)
    lw      s0,  8(t0)
    lw      s1, 12(t0)
    lw      s2, 16(t0)
    lw      s3, 20(t0)
    lw      s4, 24(t0)
    lw      s5, 28(t0)
    lw      s6, 32(t0)
    lw      s7, 36(t0)
    lw      s8, 40(t0)
    lw      s9, 44(t0)
    lw      s10, 48(t0)
    lw      s11, 52(t0)

    ret

    .size benvisor_return_from_linux, . - benvisor_return_from_linux
