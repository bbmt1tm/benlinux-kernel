# CLIC24 ownership: the real fork, and the A/B test

## What the timer-driver source revealed
- The kernel `esp32p4-timer` driver is bound in DT to **hwirq 24**
  (`interrupts-extended = <0x01 0x18>`), registers `request_percpu_irq`→24,
  clears `INT_ST`, and reprograms systimer alarm 1 itself in `set_next_event`.
- It **never writes the IPC `mtimecmp`** that BenVisor's ISR polls. So BenVisor's
  line-7 injection bridge has been running against a stale `mtimecmp` and was
  **never the real tick path**.
- Pristine BenVisor sets CLIC24 `SHV=1`, which hijacks line 24's vector to
  `mtvt[24]` (BenVisor's ISR). The kernel's own handler for hwirq 24 therefore
  never runs. Two masters touch the same alarm/CLIC line → collision that
  detonates at the first U-mode return (HEALTH probe: `ip=0`, `MPP=0`).

## VARIANT B (this build) — kernel owns line 24  [benvisor.c only, app rebuild]
CLIC24 `SHV=1 → 0` (attr `0x01 → 0x00`). Line 24 now dispatches via the kernel's
`mtvec → do_irq →` the driver's registered hwirq-24 handler. BenVisor's `mtvt[24]`
ISR is out of the tick path; the driver is the single, consistent owner.

**Reading the result (IMPORTANT — success looks different in B):**
- BenVisor's ISR no longer runs, so HEALTH `isr=` stays **0** and there is no `+`
  heartbeat. That is EXPECTED, not a failure.
- SUCCESS = `tx` advances past 783 and you get a `/bin/sh` prompt; the kernel's
  own tick keeps the scheduler alive.
- If it still freezes: capture the banner. `probe.CLIC24 ie/ip` and whether the
  kernel printed any `esp32p4-timer` interrupt activity tell us if line 24 reached
  the kernel handler at all.

Note: BenVisor's ISR also did IPC console drain. With it out of the path, console
TX now relies solely on Core 0's relay loop (which already drains `tx`), so console
should be unaffected. If console TX stalls but the kernel is alive, that's the only
expected side effect and is cosmetic for this test.

## VARIANT A (fallback, only if B fails) — BenVisor stays middleman  [kernel rebuild]
Keep CLIC24 `SHV=1` (BenVisor services line 24) and **rebind the kernel to the
line BenVisor injects**: in `esp32p4.dts`, `interrupts-extended = <0x01 0x18>` →
`<0x01 0x07>` (24 → 7). Then BenVisor's existing line-7 injection lands on the
kernel's registered handler. This keeps BenVisor's console-drain/heartbeat duties
but requires a DTB rebuild. Do this ONLY if B shows line 24 can't be owned by the
kernel directly (e.g. P4 CLIC won't deliver a non-vectored systimer line to mtvec).

## Why B first
B matches the driver exactly as written, is one byte, app-only, no kernel rebuild,
and is now the most likely-correct design rather than a coin flip. A is the bigger
change and only makes sense as a fallback.
