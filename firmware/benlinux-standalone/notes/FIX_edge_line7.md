# Fix: line 7 EDGE-triggered (break the stuck-pending livelock)

## Progress this round (TARGET1-threshold build)
For the FIRST time the tick injection DID something at /bin/sh:
- isr rate collapsed from ~10000/poll to **271**, then froze (every prior run kept
  isr~5000+ right up to the freeze). isr total 22673 vs ~45000 before.
- This means line 7 is now actually being injected (TARGET1 became a real deadline
  once the kernel's set_next_event ran) and the kernel started taking line-7 traps.

## Why it collapsed
BenVisor sets clicintip[7]=1 artificially. The kernel takes line 7 and its handler
clears the SYSTIMER INT_ST — but that does NOT clear clicintip[7] (BenVisor is the
artificial source; the kernel can't know to clear that CLIC bit). With line 7
LEVEL-triggered, clicintip[7] stayed 1, so the kernel re-took line 7 endlessly,
starving BenVisor's line-24 ISR (isr→271) until wedged.

## The fix (benvisor.c only, 1 byte — app rebuild)
Line 7 attr 0x01 → 0x03 (SHV=1, **EDGE** trig=01). On a CLIC edge interrupt, taking
the vectored trap AUTO-CLEARS clicintip in hardware. So BenVisor injects once per
deadline → kernel takes it once → IP auto-clears → fires exactly once. This is the
natural model for an injected tick and matches real timer-interrupt behaviour.

Line 24 stays SHV=1 level (BenVisor's own heartbeat path, unchanged and working).

## How to read the result
- SUCCESS: lives past /bin/sh, jiffies advance, shell prompt. isr stays healthy
  (~10000/poll) past /bin/sh — no collapse. C7 ip toggles 1→0 cleanly.
- isr collapses to a low number again / livelock: edge did NOT auto-clear IP on this
  P4 (vectored take doesn't clear edge latch here). Then the kernel handler must clear
  clicintip[7] itself, OR we go A-chain (BenVisor jumps into _start_trap directly,
  no separate line-7 pending bit to manage).
- Freezes with isr=0 immediately and no collapse: line 7 stopped being injected —
  check TARGET1 advancing.

## Decision context
This is one byte and directly targets the observed stuck-IP livelock using the CLIC's
own edge auto-clear. If it doesn't take, the A-chain (jump straight into _start_trap)
is the next move — it sidesteps the line-7 pending-bit handshake entirely.
