# Option A: software self-rearm of line 24 (PoC — does a shell appear?)

## Why
PROVEN: in U-mode the P4 CLIC won't latch clicintip[24] from the systimer->INTMTX
hardware path (tested level AND edge). But a DIRECT MMIO write to clicintip works in
U-mode (that's how line-7 injection succeeds). So BenVisor re-asserts clicintip[24]=1
itself before mret, instead of waiting for the systimer to re-trigger it. Line 24 then
re-fires regardless of privilege, keeping the heartbeat — and the line-7 kernel tick it
injects — alive after the kernel drops to U-mode at /bin/sh.

## Change (benvisor_asm.S only, app rebuild)
In the ISR re-arm tail, after re-enabling TARGET1_EN, add:
    sb clicintip[24]=1   (0x20801060)
Built on the working build (TARGET1-threshold inject, line7 vectored+edge, line24 edge).

## KNOWN FLAW — this is a PoC, not the fix
Line 24 is priority 7, so it re-fires IMMEDIATELY on every mret => Core 1 spends ~100%
of its time in BenVisor's ISR; the kernel/userspace get very little CPU. Expected
outcomes:
- BEST CASE: the kernel still receives its line-7 ticks, so it makes (slow) forward
  progress and a /bin/sh prompt appears (possibly sluggish). That CONFIRMS the kernel
  side is fully working and the only remaining issue is an efficient U-mode tick.
- LIKELY: isr counter now stays NONZERO past /bin/sh (heartbeat alive!) — the headline
  signal to watch. mepc should keep roaming (kernel running). Whether a prompt actually
  prints depends on how much CPU the kernel gets between BenVisor re-entries.
- If it hangs hard with isr pinned and no kernel progress: the self-rearm starved the
  kernel completely. Still informative.

## Next: Option B (efficient U-mode tick) — FRESH CHAT
See CONTEXT_CHECKPOINT.md. Strongest candidate: CLINT mtimecmp (standard RISC-V machine
timer, not U-gated; kernel has native timer-riscv path). Also: CLIC clicintattr mode
field, or routing systimer to a non-U-gated CLIC line.

## What to capture from the run
- Does isr stay nonzero AFTER "Run /bin/sh"? (heartbeat survived U-mode = the win)
- Does a shell prompt / any post-/bin/sh output appear? Does it echo the \r pokes?
- mepc values post-/bin/sh (kernel making progress vs pinned).
