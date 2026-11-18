# PATCH 10 — User-return state probe (kernel CI patch, entry.S)

## Why
Static disasm of the kernel Image (loaded at 0x48200000) localized the freeze:

- The 3 WFI sites (0x48200040, 0x482027cc, 0x484c1328) are all NOP'd by the
  loader; the idle loop is a clean `fence; wfi(->nop); ret`. Idle is not the bug.
- `mepc` at freeze (0x484b95f4 etc.) is inside `memcpy` — i.e. just wherever the
  timer caught the kernel. It is NOT a stuck PC. Chasing it was a dead end.
- There is exactly ONE `mret` in the kernel, at 0x484c7ab0. The trap exit checks
  `mstatus.MPP` (mask 0x1800): MPP!=0 -> returning to kernel (M-mode); MPP==0 ->
  returning to USER, and only then sets `mscratch` = kernel sp.

So this kernel is **M-mode kernel + U-mode userspace (NOMMU)**. The freeze hits at
"Run /bin/sh" — the FIRST return to U-mode. The timer fires 45,776 times in M-mode,
then the instant the hart is in U-mode, line 24 never vectors again (isr=0, bail=0,
INT_ST=0x2 latched, INTMTX=24 intact, mtvec intact).

Ruled out: MIE (M-mode interrupts ignore mstatus.MIE while the hart is in U-mode),
MPIL (init pt_regs.cause is memset 0 -> mil 0), routing, WFI/idle, the bail/storm.

Leading hypothesis: the P4 CLIC is not delivering the M-mode timer interrupt to the
hart while it runs in U-mode — either BenVisor's CLIC24 config is missing whatever the
P4 needs for cross-privilege delivery, or it's a P4 CLIC limitation. PATCH 10 captures
the exact state at the U-mode crossing so we stop guessing.

## What it captures
On every trap return, just before `mret`, write 4 words to the IPC probe scratch
(absolute uncached 0x8ff3c124 = IPC base 0x8ff3b804 + struct offset 0x920):
  probe[0] = mstatus being restored   (MPP[12:11]: 0=user return, 3=kernel)
  probe[1] = mcause  being restored   (MPIL[23:16] = post-mret CLIC level)
  probe[2] = live CLIC24 ctrl @0x20801060  ([ip][ie][attr][ctl])
  probe[3] = live mintthresh  @0x20800008
The LAST values latched before the freeze are the user-crossing state. Core 0 prints
them every poll (`MPP=` and `c24=` on the [H] line) and in the freeze banner.

## Where to insert (arch/riscv/kernel/entry.S)
In the trap-return path (`ret_from_exception` / `restore_all`), AFTER the PATCH-9
`csrw CSR_CAUSE, sN` line and BEFORE the general-purpose register reload (the run of
`REG_L x1, PT_RA(sp)` ...). At that point `sp` still holds the pt_regs pointer and
t0/t1/t2 (x5/x6/x7) are about to be reloaded, so clobbering them is safe.

```asm
    /* PATCH 10: capture U-mode-crossing state to IPC probe @0x8ff3c124 */
    li   t2, 0x8ff3c124
    lw   t0, PT_STATUS(sp)       /* observed offset 128 */
    sw   t0, 0(t2)
    lw   t0, PT_CAUSE(sp)        /* observed offset 136 */
    sw   t0, 4(t2)
    li   t1, 0x20801060          /* CLIC24 ctrl */
    lw   t0, 0(t1)
    sw   t0, 8(t2)
    li   t1, 0x20800008          /* mintthresh */
    lw   t0, 0(t1)
    sw   t0, 12(t2)
```

Use the `PT_STATUS` / `PT_CAUSE` asm-offset macros if available; the raw offsets
observed in the built Image are 128 and 136. Assembles clean as rv32ima_zicsr.

If Core 0 prints `PROBE addr != 0x8ff3c124` at boot (allocator moved IPC), update the
`li t2, ...` constant to the value it prints.

## How to read the result
- `probe.mstatus` MPP at freeze: expect **0 (USER)** — confirms the crossing.
- `probe.CLIC24`: if `ie=0` something disabled the line; if `ip=0` it never latched;
  if `attr`/`ctl` changed, the kernel reconfigured it. If `ie=1 ip=1` and unchanged,
  the line is armed but not delivered → U-mode delivery problem (config or HW).
- `probe.mcause` MPIL: if nonzero, the user mret raised the running level (mask).
- `probe.thresh`: expect 0; if raised, threshold is masking the line.

## Parallel: question for the research chat
Confirm against the P4 TRM / CLIC reg spec: does an M-mode CLIC interrupt
(clicintattr mode=11) preempt the hart while it is executing in U-mode? Is there a
separate U-mode delivery enable or a per-privilege `mintthresh`/`uintthresh`? Does
entering U-mode change the effective interrupt level gate? This determines whether the
fix is a CLIC24 config change in benvisor.c or running userspace in M-mode.
