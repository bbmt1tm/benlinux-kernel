# PoC: vectored line-7 kernel tick (HARDCODED — throwaway test)

## The finding that led here
Variant B (line 24 SHV=0) froze EARLIER (~0.145s, not /bin/sh) with the kernel's
own diag showing `cfg=0x48695f44` (CLIC corruption) and `C24=...0101` (ip=1 LATCHED
but never delivered). That proved: **on this P4 CLIC, SHV=0 (non-vectored) lines
latch their pending bit but are NOT delivered to mtvec.** Only the VECTORED (mtvt)
path delivers.

That also explains the original bug: BenVisor's line-7 injection (the kernel's tick)
was ALSO SHV=0 — so the kernel NEVER received a single timer interrupt. Linux booted
straight-line on the free-running CSR clocksource and froze the instant it first
needed a scheduler tick (/bin/sh blocking). The 45k "isr" counts were BenVisor's own
heartbeat firing into a void.

## What this PoC changes (4 files, APP-ONLY rebuild — no kernel rebuild)
1. benvisor.c — CLIC line 7 SHV 0→1 (attr 0x00→0x01): make the kernel-tick line
   VECTORED so it actually delivers.
2. benvisor_asm.S — mtvt[7] = 0x484c78dc (kernel _start_trap). When BenVisor injects
   line 7, the hart vectors here → kernel do_irq → timer handler.
3. esp32p4_dtb.h + esp32p4.dts — rebind timer IRQ from hwirq 24 → 7
   (interrupts-extended <0x01 0x18> → <0x01 0x07>), so the kernel's registered
   handler matches the line BenVisor injects. Single-byte DTB change, regenerated
   with dtc (verified byte-for-byte identical to the shipped DTB except that byte).

Line 24 stays SHV=1 → BenVisor ISR (pri 7); line 7 is pri 6, so it can't preempt the
running ISR — it fires cleanly AFTER BenVisor's mret. No nesting hazard.

## HARDCODE WARNING
mtvt[7] = 0x484c78dc is _start_trap for the kernel currently on the SD card
(md5 873c397aa1fdcd1eb139cef15d10267e, the PATCH-10 build dated May 20).
VERIFIED against that exact Image: head.S writes mtvec=0x484c78dc|3, and
0x484c78dc is the _start_trap mscratch-swap entry. (It happens to match the
earlier Apr-19 build too, because PATCH 10 inserted into the restore path AFTER
the entry — the single mret moved 0x484c7ab0->0x484c7ae8 but _start_trap did
not.) It WILL break on any FUTURE kernel rebuild that changes entry.S above
_start_trap.
This build is a throwaway to PROVE vectored line-7 delivers the tick. If it works,
the durable fix carries _start_trap via the DTB or an early IPC write instead of a
constant.

## How to read the result
- SUCCESS = kernel tick finally arrives: jiffies advance, /bin/sh gives a live
  prompt, and (unlike Variant B) the system stays alive past 0.145s and past
  /bin/sh. HEALTH `isr=` still climbs (BenVisor ISR on line 24 still runs its
  heartbeat) — but now the injected line-7 also reaches the kernel.
- If it freezes at /bin/sh exactly as the original: line 7 vectored still isn't
  delivering to the kernel handler — capture the banner; check whether the kernel
  printed any timer-IRQ activity and whether C7 (0x2080101C) shows ip latched.
- If it freezes EARLY like Variant B (~0.145s, cfg=0x48695f44): the act of the
  kernel taking a vectored trap is itself triggering the CLIC corruption — that
  would point at the mtvt[7] target/alignment, not the delivery path.
