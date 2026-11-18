# Fix: inject the kernel tick using the kernel's real deadline (ST_TARGET1)

## Root cause (fully evidenced this round)
The PoC proved line 7 was now vectored (C7 attr 0xc0→0xc1) and cfg stayed healthy
(0x00000007), yet it froze identically at /bin/sh. Reason: **line 7 was never fired.**

benvisor.c:102 inits IPC mtimecmp = 0xFFFFFFFF_FFFFFFFF and the kernel NEVER writes
it (the timer driver programs the systimer TARGET1 directly, not IPC mtimecmp). So
BenVisor's ISR condition `time >= mtimecmp` was `time >= 2^64-1` — never true. The
kernel has never received a single timer interrupt; it boots straight-line on the
free-running CSR clocksource and freezes the instant init needs a scheduled wakeup.

## The fix (benvisor_asm.S only — app rebuild)
BenVisor's mtvt[24] ISR now compares the live **systimer counter** against the
kernel's **ST_TARGET1** deadline and injects line 7 when due:

- Clock domain matters: CSR `time` is 360 MHz (clocksource); the systimer counter
  and TARGET1 are 16 MHz (the alarm). The compare MUST be counter-vs-TARGET1, both
  16 MHz — NOT CSR time. (The old dead code wrongly used CSR time.)
- Latch counter: write UNIT0_UPDATE (1<<30) to 0x500E2004; read 0x500E2040(hi)/44(lo).
- Deadline: read TARGET1 0x500E2024(hi)/28(lo).
- If counter >= target → set CLIC line 7 IP=1. Line 7 is vectored (PoC change) →
  mtvt[7]=_start_trap → kernel do_irq → hwirq-7 handler = the real tick.

Builds on the PoC (line 7 SHV=1, mtvt[7]=0x484c78dc, DT rebind 24→7). IPC mtimecmp
and benvisor_mtimecmp_ptr are now unused but left defined (harmless).

## Why this is decoupled from the kernel's alarm fiddling
The kernel's set_next_event disables/re-enables the systimer alarm. BenVisor's own
100us heartbeat drives CLIC24 independently and re-arms the alarm every tick, so it
ALWAYS sees the current TARGET1 and injects on the kernel's cadence regardless of the
kernel's enable-bit toggling. This sidesteps the original two-masters fragility.

## How to read the result
- SUCCESS: lives past /bin/sh, jiffies advance, shell prompt. C7 ip should toggle
  (set by BenVisor, cleared when the kernel takes it). isr= still climbs (line 24).
- Freezes at /bin/sh again: capture the banner. Key new things to look at —
  * Does C7 (0x2080101C) show ip=1 latching but never clearing? → line 7 is being
    injected but the kernel handler isn't running (vectored delivery / hwirq-7
    registration issue) → then we try the A-chain (jump into _start_trap).
  * Is TARGET1 (now readable) sane / advancing? If TARGET1 stays at boot value, the
    kernel's set_next_event isn't running (no clockevent) — different problem.
- Freezes early w/ cfg=0x48695f44: vectored line-7 trap trips CLIC corruption.
