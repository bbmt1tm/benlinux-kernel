# Fix: line 24 EDGE-triggered too (keep BenVisor's heartbeat alive through U-mode)

## HUGE progress in the previous run (edge line 7)
First-ever evidence the kernel is running on a real timer tick:
- mcause cause changed 0x18(24) → **0x07** — the kernel TOOK the injected line-7 tick.
- MPP=3 boot polls with mepc roaming across many kernel functions (0x4820bab8,
  0x482a06b4, 0x4822e874, ...) = **the scheduler is running**, not stuck in idle.
- [RX:01] + tx_head advanced 783→784 = kernel consumed input and produced output.
So edge line 7 fixed the kernel-tick delivery+ack. Confirmed working.

## Remaining freeze, now fully isolated
It still freezes at /bin/sh. At that exact point MPP flips 3→0 (kernel→USER), and
BenVisor's OWN line-24 heartbeat stops (isr→0, saved_mcause=0xb8000018=cause 24).
At freeze: INT_ST=0x2 (alarm fired) but C24 ip=0 (NOT latched).

This is the ORIGINAL bug, now isolated with everything else working: line 24 is
LEVEL-triggered, and at the first U-mode return the systimer level fails to latch
clicintip[24]. BenVisor's ISR never re-enters → heartbeat dies → no more line-7
injection → kernel tick dies too.

(We "tried edge on 24" many builds ago, but that was inconclusive: the kernel tick
was dead then regardless, so /bin/sh would freeze anyway and mask any line-24 result.
Now the tick chain works, so this is the first clean test of edge-on-24.)

## The fix (benvisor.c only, 1 byte — app rebuild)
Line 24 attr 0x01 → 0x03 (SHV=1, **EDGE**), matching the line-7 fix that just worked.
EDGE latches clicintip[24] on the alarm's rising edge regardless of privilege and
auto-clears on vectored ISR entry. The ISR already clears INT_ST early (source→low)
and re-arms last (alarm fires 100us later → fresh rising edge → next tick) — a clean
edge per tick, verified.

Now BOTH lines are edge: 24 (BenVisor heartbeat) and 7 (kernel tick).

## How to read the result
- SUCCESS (the goal): isr stays alive past /bin/sh (no drop to 0), MPP toggles
  0↔3 as the shell runs and is preempted, and you get a **/bin/sh prompt** that
  echoes the \r pokes. That's a live Linux shell.
- isr still dies at U-mode entry (ip=0): this P4's CLIC genuinely will not latch the
  systimer→CLIC24 edge while in U-mode → the systimer-as-CLIC-source can't survive
  U-mode at all. Then the move is the A-chain: stop relying on line 24 being
  re-delivered; instead drive everything from a path that doesn't depend on CLIC24
  latching in U-mode (e.g. inject line 7 from a source that does latch, or run the
  heartbeat off a mechanism immune to the privilege gate).
- Boots further / different freeze: capture and we read the new state.
