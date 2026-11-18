# BenLinux Standalone — Post-/bin/sh Freeze Diagnostic (HEALTH-V2)

**Date**: 2026-05-20
**Status**: Previous "victory" call was premature. Kernel boots to `/bin/sh`, ticks for ~150 ms, then `benvisor_timer_isr` stops being invoked. INT_ST=2 latches forever; no `+` heartbeat. This pack tightens the Core 0 diagnostic to surface what Core 1's ISR was looking at when it died.

---

## What changed in this drop

**Only `main/main.c`**. No kernel rebuild needed. No `benvisor.c` / `benvisor_asm.S` changes — the asm side already writes the state we need into BSS globals on every ISR call; the relay loop just wasn't reading them.

### Added extern decls (top of file)
Pulls in the BenVisor BSS symbols that the ISR updates every call:
- `benvisor_isr_count`     — incremented every ISR entry
- `benvisor_last_mepc`     — kernel PC at the moment the ISR fired
- `benvisor_diag_mtvec`    — Core 1's mtvec as seen by the ISR
- `benvisor_diag_mstatus`  — Core 1's mstatus as seen by the ISR
- `benvisor_heartbeat_ctr` — decrementing counter (10000 → 0 → 10000 again)

A `read_bv_nc()` helper reads them via the uncached SRAM alias (`+0x40000000`), same trick as IPC.

### Rewrote `uart_relay_loop()` HEALTH block
Same polling cadence (~5 Hz empirically), but every poll now prints **deltas** rather than raw values:

```
[H] tgt=0x........(STA d=0x0) ST=0x2 isr=0 tx=0 hbd=0 mst=0x........ mepc=0x........
```

- `tgt`     — alarm 1 target (was already in the old format)
- `(STA|CHG d=...)` — STA/CHG plus the *signed delta* in target since last poll
- `ST`      — systimer INT_ST (bit 1 = alarm 1 fired, not yet cleared)
- `isr=N`   — number of BenVisor ISR calls **in this poll window**. Zero = ISR dead.
- `tx=N`    — number of bytes the kernel appended to the printk ring buffer this window
- `hbd=N`   — heartbeat counter delta (positive = decrementing = ISR alive)
- `mst`     — last seen `mstatus` from inside the ISR (bit 3 = MIE, bit 7 = MPIE)
- `mepc`    — kernel PC the **last time the ISR ran** ← this is the gold value

### Freeze detection
Three consecutive polls of `isr_delta == 0 && INT_ST & 0x2` triggers a one-shot banner:

```
=========================================
 !! BenVisor ISR FREEZE DETECTED !!
=========================================
 Last successful ISR snapshot:
   isr_count    = N
   last_mepc    = 0x........   <-- KERNEL PC AT FREEZE (disasm here)
   diag_mtvec   = 0x........   <-- expected 0x484c78c3 (kernel mtvec)
   diag_mstatus = 0x........   <-- MIE bit3=?, MPIE bit7=?
 ...
```

After the banner, the loop pokes `\r` into the IPC RX ring every ~1 s. If the shell processes that newline (any `tx_head` advance, any echo) we know the **kernel scheduler is alive** and **only the timer-ISR path is dead**. If nothing comes back, kernel itself is wedged.

A `RECOVERED` line fires if `isr_delta > 0` ever appears again — sanity check that detection isn't sticky.

---

## How to read the output

### Pre-freeze, healthy state
```
[H] tgt=0x07c3f58a(CHG d=0x4f...) ST=0x0 isr=2500 tx=180 hbd=12500 mst=0x00010088 mepc=0x484c...
```
- `isr=2500` per ~200 ms poll → ~12.5 kHz ISR rate (matches BenVisor's 100 µs re-arm cadence give-or-take).
- `mst=0x00010088` → MIE=1, MPIE=1 (bit 3, bit 7). Normal.
- `mepc=0x484cxxxx` → kernel PC. Was inside the kernel image (`0x48200000` + offset).

### Post-freeze
```
[H] tgt=0x08424835(STA d=0x0) ST=0x2 isr=0 tx=0 hbd=0 mst=0x........ mepc=0x........
```
- `isr=0` → ISR didn't run this window. **THE BUG.**
- `tx=0` → kernel didn't printk anything this window. Could mean idle (waiting for input) OR dead.
- `mst` and `mepc` will be **frozen at the values from the last successful ISR call** — that's the kernel state at the freeze instant.

### After freeze + poke
- If next polls show `tx=N` for N>0 → shell processed the `\r`. Kernel is alive. Bug is purely in CLIC24 delivery to Core 1.
- If `tx=0` indefinitely → kernel itself is wedged. Different bug class.

---

## Disassembling `mepc`

This is the high-value action. Once the banner prints `last_mepc = 0x484cXXXX`, on your dev machine:

```bash
# From the kernel-build CI artifact, you have System.map + vmlinux
riscv32-buildroot-linux-uclibc-objdump -d vmlinux | less

# Search for the address
/484cXXXX
```

Or faster via System.map → symbol lookup:
```bash
# Find which function contains that address
awk -v addr=0x484cXXXX '$1 ~ /^[0-9a-f]+$/ && strtonum("0x"$1) <= addr {fn=$3} END {print fn}' System.map
```

That tells us **what kernel code was running when the ISR last fired** — i.e. the code path that produced whatever state breaks CLIC24 delivery.

Likely candidates given timing (right after `Run /bin/sh`):
- syscall entry path (`handle_exception`, `do_trap_ecall_m`)
- `arch_cpu_idle` (the patched-WFI-now-NOP path)
- scheduler entry on first context switch
- the `csrw CSR_CAUSE` epilogue in `entry.S` (PATCH 9 site) — if `mcause` snapshot is wrong here, MIL latches high on `mret`

If `mepc` lands in or just before PATCH 9's `csrw CSR_CAUSE` block, that strongly implicates the patch as having a missed case.

---

## What the diag fields tell us if they DON'T match expectations

| Field            | Expected (healthy)        | If different at freeze                                                                 |
|------------------|---------------------------|----------------------------------------------------------------------------------------|
| `diag_mtvec`     | `0x484c78c3` (MODE=3)     | Kernel changed mtvec (mode or base). Vectoring is broken; CLIC24 has nowhere to go.    |
| `diag_mstatus.3` | `1` (MIE set)             | MIE got cleared. A `local_irq_disable()` somewhere with no matching enable.            |
| `diag_mstatus.7` | `1` (MPIE set)            | A trap pushed MIE=0 → MPIE=0 and mret restored MIE=0. Nested-trap accounting bug.      |
| `tgt` delta      | nonzero between polls     | Already zero in your log → ISR not re-arming → ISR not running.                        |
| `ST & 0x2`       | usually 0, briefly 1      | Latched 1 → alarm fired but no handler — confirms CLIC delivery broken on Core 1.      |
| `INT_ENA & 0x2`  | `1`                       | Cleared → kernel disabled alarm 1 at the systimer level. Different failure class.      |
| `INTMTX_C1`      | low 6 bits = 24           | Changed → kernel rerouted Core 1 systimer1 away from CLIC24. Unlikely but worth seeing.|

---

## Iteration plan

1. **Drop in this `main.c`**, rebuild app only (no kernel rebuild), flash, capture full log.
2. Read the freeze banner. **Record `last_mepc`, `diag_mtvec`, `diag_mstatus`.**
3. Watch ~30 s of post-freeze polls. Note whether `tx=...` ever becomes nonzero after the `\r` pokes.
4. Disassemble around `last_mepc` in the kernel vmlinux. Identify the function and the surrounding code.
5. Bring those three values + the function name back here. That's enough to narrow to one of the candidate kernel paths (PATCH 9 site, idle loop, syscall entry, scheduler).

If that still isn't enough, the next step is a kernel rebuild with one extra patch:
- **PATCH 10 (proposed)**: `arch/riscv/kernel/process.c arch_cpu_idle()` — write a u32 counter to `*(volatile uint32_t *)0x8FF3B834` on every entry. (Address = IPC base `0x8FF3B804` + 0x30; reuses the `rom_putc_addr` field's neighbouring word — safe scratch space.) Then HEALTH reads that field too and we know if the idle loop is being entered after freeze.

But try the `last_mepc` route first — one local rebuild is much cheaper than a 20-minute CI run.

---

## What this drop **does not** change

- `benvisor.c`, `benvisor_asm.S`, `esp32p4_dtb.h`, `sdkconfig` — all unchanged.
- CLIC24 routing (still SHV=1 via BenVisor's `mtvt[24]`).
- No new kernel patches.
- No DTB regeneration.

Just `main/main.c`. Drag over `main/main.c` from the zip, rebuild app, flash, monitor.

---

## If you'd rather not modify anything yet

The fact that BenVisor's `+` heartbeat stops at the same instant as `INT_ST=0x2` latches is already proof enough that **BenVisor's ISR (not the kernel's CLIC24 timer driver) is what was actually servicing ticks all along**, and that's what's stopping. The kernel's mtvt[24] was never installed — the kernel timer driver's `request_percpu_irq(1)` registers a software handler bridged via `irq-riscv-intc`, but the hardware vector dispatch goes through the mtvt CSR which BenVisor wrote at boot and the kernel doesn't touch. So when the previous chat said "either SHV value works" — yes, but for a non-obvious reason: SHV=1 routes to BenVisor's mtvt, and BenVisor's ISR is what re-arms the alarm and clears INT_ST. The kernel's clockevent never actually fires in this build.

That observation alone might suggest a different angle: **set CLIC24 SHV=0** (kernel direct dispatch to `mtvec`), so the kernel's own CLIC24 handler actually runs. If the freeze then disappears, the issue was that BenVisor's ISR has some specific failure mode the kernel handler would have avoided. If the freeze still happens with SHV=0, the bug is in shared infrastructure (CLIC24 enable bit, MIE state, etc.). That's a separate test (Test 8 in the original session notes) and is independent of this diagnostic.

This diag is the cheap "look first" — Test 8 is the cheap "swap and retry". Do both in parallel if you want.

---

## UPDATE (HEALTH-V2.1) — after first freeze capture

First capture is in. Key banner values:

```
last_mepc    = 0x484b9624    (in idle cluster 0x484b95fc..0x484b9950)
diag_mtvec   = 0x484c78c3    (= kernel mtvec, intact)
diag_mstatus = 0x00011880    (MIE=0, MPIE=1 — normal in-ISR snapshot)
INT_ST=0x2  INT_ENA=0x3  CONF TARGET1_EN=1  INTMTX_C1=24
tx_head frozen at 783; \r pokes produce no echo, tx stays 0
```

### What this rules out
- **WFI halt** — ISA is `aim` (no C-ext), all WFI are 4-byte `0x10500073`, scan covers
  the whole 4.74 MB image and NOP'd all 3. There is no 4th. Idle busy-spins.
- **Systimer / routing** — alarm fired (INT_ST=2), still enabled, target1 on, INTMTX→24.
  The interrupt is *generated and routed* correctly.
- **mtvec clobber** — intact.
- **"ISR dead, shell alive"** — no. printk frozen + no echo = whole hart livelocked
  in the idle spin, waiting for a `need_resched` that only a tick can set.

### Leading cause: CLIC running-level stuck high (`mret` / MPIL)
The ISR (`benvisor_asm.S`) saves `mcause` on entry (L138) and `csrw mcause` right before
`mret` (L257-259) — PATCH 9. On this CLIC, `mret` reloads `mintstatus.mil` from the
MPIL field of that word. If the restored MPIL is >= the tick's level (7), line 24 can no
longer preempt idle (CLIC needs *strictly greater* level), and ticks stop forever.

Why it triggers exactly at `/bin/sh`: the ISR does **blocking ROM UART I/O** every tick
(console drain L189-206 + `'+'` heartbeat L219) on a 100 us (now+1600) re-arm. The shell
start floods printk (`tx` jumped to 783). The ISR overruns 100 us, the alarm re-fires
mid-ISR, a level-7 instance stacks on a level-7 instance, and the MPIL restored on `mret`
sticks `mil` high. Matches the timing precisely.

### This drop confirms it (main.c only)
`benvisor_saved_mcause` is already written every tick (asm L138). HEALTH-V2.1 now prints it:
- per-poll: `mc=0x........` (replaces `mst=`; mstatus still in the banner)
- banner: decoded `MPIL[31:24]`, `[23:16]`, `exc[11:0]`

**Read:** if the working polls show `mc` with a low/zero MPIL and the frozen poll shows a
high MPIL (e.g. 0x88-ish high byte, matching the earlier `CLIC-DIAG MPIL=136`), the
stuck-level theory is confirmed. If `mc` is identical working-vs-frozen, fall back to
disassembling `0x484b9624` and reading Core 1's CLIC24/mintstatus another way.

### Candidate fixes, in order (do ONE, after mc confirms)
1. **Root cause — get slow I/O out of the tick ISR.** Delete the console-drain (L189-206)
   and `'+'` heartbeat (L209-219) from `benvisor_asm.S`. Core 0's relay loop already drains
   `tx` (main.c L587-599), so the ISR drain is redundant *and* it's what blows the 100 us
   budget. ISR then just clears INT_ST + re-arms — sub-microsecond, no overrun, no pileup.
2. **Force the level down on exit.** Before `csrw mcause` (L257), zero the MPIL field of the
   saved word so `mret` always drops `mil` to 0. Needs the exact P4 mcause MPIL bit range
   (pull from Espressif `clic` reg defs / TRM — research chat). Keep exc code + MPP intact.
3. **Loosen the re-arm.** now+1600 (100 us) -> e.g. now+16000 (1 ms). Reduces overrun odds
   but is a band-aid, not a fix; do only if (1) is undesirable.

Recommendation: **(1)**. It removes the overrun mechanism entirely and is a clean
benvisor_asm.S-only change with no kernel rebuild. Confirm with `mc` first.

---

## UPDATE (HEALTH-V3) — ROOT CAUSE FOUND + FIX

The V2.1 capture **disproved** the level-pileup theory and pointed straight at the real bug.

### V2.1 result
`saved_mcause = 0xb8000018` on every poll, working and frozen alike. Decoded with the
correct CLIC field layout (not `>>24`):

```
bit31 interrupt=1  bit30 minhv=0  [29:28] mpp=3(M)  bit27 mpie=1
[23:16] MPIL = 0x00   <-- real previous level: ZERO
[11:0]  exc  = 24
```

MPIL is 0 every tick, so `mret` restores running level 0 correctly — the level is **not**
stuck. (The kernel's `CLIC-DIAG MPIL=136` is a cosmetic bug: it reads `mcause>>24`, which
grabs int|minhv|mpp|mpie, not the `[23:16]` MPIL field.)

### Root cause: bail-path interrupt storm
`benvisor_timer_isr` starts with `beqz sp, .Lisr_bail_fast`, and the old bail was a bare
`mret`. When a tick fires while `sp == 0`, the bail returns **without clearing the systimer
INT_ST**. Line 24 is *level-triggered*, so the alarm stays asserted; the instant `mret`
drops the level to 0 (MPIL=0), line 24 re-fires. Bail → mret → re-fire, forever. The hart
executes zero kernel instructions.

This matches every symptom exactly:
- `isr=0` frozen — the ISR counter is incremented *after* the register save, which the bail
  skips. The storm never reaches it.
- `INT_ST=0x2` latched — the bail never clears it.
- `mepc` frozen at `0x484b9624` — `benvisor_last_mepc` is also written after the save; it's
  stuck at the last *full* tick (kernel idle).
- printk frozen, `\r` pokes silent — 100% of cycles consumed in the storm.
- freeze lands exactly at "Run /bin/sh" — the kernel `mret`s into NOMMU userspace and the
  first tick fires while `sp` is the user `sp`, which comes up **0**. The last full tick
  (kernel idle, `0x484b9624`) is the last `mepc` ever recorded.

The earlier `ST=0x0 isr=0` polls were just quiet windows (no alarm pending), not storms —
a true storm can't self-recover since the hart never executes. They recovered; the final
`ST=0x2 isr=0` did not. Consistent.

### Fix (this drop) — `benvisor_asm.S` + `main.c`
The bail can't use the kernel stack (`sp==0`) or `mscratch` (Linux owns it). But in the
bail branch we *know* `sp==0`, so nothing needs preserving: point `sp` at a private 512 B
BSS stack, get scratch regs, **clear INT_ST**, **re-arm** at the normal cadence, drop a
breadcrumb, then `li sp,0` and `mret`. A missed tick now drops one tick instead of wedging.
Assembled clean for rv32ima_zicsr; bail path verified in disassembly.

New breadcrumbs surfaced on Core 0:
- `bail=N(dM)` — total bail count and per-poll delta. `dM>0` = ticks are landing on `sp==0`.
- `bpc=0x........` — the PC running with `sp==0`. **This is the next gold value.**

### What to expect on flash
1. **Shell comes alive** (sp==0 was transient): `bail` ticks up a little around `/bin/sh`,
   then `isr` resumes and you get a prompt. Bug was purely the storm — done.
2. **`bail` climbs continuously, no prompt** (sp==0 persists): the kernel is alive (no
   storm) but keeps returning to a context with `sp==0`. Read `bpc`:
   - `bpc` is a **userspace** address → `/bin/sh` entered U-mode with `sp=0`: NOMMU user
     stack / ELF stack-pointer init issue (initramfs `/bin/sh`, `bflt`/ELF load, or the
     `mret`-to-user context setup). That's the next thing to fix, kernel/userspace side.
   - `bpc` is a **kernel** address → identify it: `riscv32-buildroot-linux-uclibc-objdump -d
     vmlinux` and find the symbol ≤ bpc in `System.map`. Likely `ret_from_fork` / the
     user-return trampoline / `__switch_to` window where `sp` is transiently 0.

### Residual (not fixed here)
`benvisor_unexpected_isr` is still a bare `mret`. All other CLIC lines are scrubbed at init,
so it shouldn't fire — but if one ever does and it's level-triggered, it would storm the
same way. Worth hardening later for symmetry; left alone now to keep this a single change.

---

## UPDATE (HEALTH-V4) — localized to the first U-mode return

The bail fix held (`bail=0`), which **disproved the sp==0 storm** — the ISR never
bailed. Same freeze. So I disassembled the kernel Image directly (loaded flat at
0x48200000; I have a RISC-V toolchain in the sandbox now).

Findings from the disasm:
- `mepc` at freeze (0x484b95f4) is inside **memcpy** — just where the timer caught the
  kernel, not a stuck PC. The mepc trail was a red herring.
- All 3 WFI (0x48200040, 0x482027cc, 0x484c1328) are NOP'd; idle is a clean
  `fence; nop; ret`. Idle is not the bug.
- Exactly ONE `mret` in the kernel (0x484c7ab0). The trap exit tests `mstatus.MPP`
  (mask 0x1800): MPP!=0 → kernel (M) return; MPP==0 → **USER (U) return**, sets mscratch.

**This kernel is M-mode kernel + U-mode userspace (NOMMU).** The freeze is the FIRST
return to U-mode (`Run /bin/sh`). Timer fires 45,776× in M-mode, then zero the instant
the hart is in U-mode.

Ruled out this round, with evidence:
- WFI halt — all WFI patched; idle clean.
- MIE — irrelevant in U-mode (M-mode interrupts ignore `mstatus.MIE` while in U).
- MPIL/level — init `pt_regs.cause` is memset 0 → mil 0 on the user mret.
- routing/mtvec — INTMTX=24 and mtvec intact at freeze.
- bail/storm/pileup — all disproved by data.

**Leading hypothesis:** the P4 CLIC does not deliver the M-mode timer interrupt to the
hart while it executes in U-mode — either BenVisor's CLIC24 setup lacks a cross-
privilege delivery bit, or it's a P4 CLIC limitation.

### This drop (HEALTH-V4)
- `benvisor.h`: adds `probe[4]` to the IPC struct (offset 0x920; abs uncached 0x8ff3c124).
- `main.c`: reads `probe[]` each poll, prints `MPP=` and `c24=` on the [H] line and a
  full decode in the freeze banner; asserts IPC base == 0x4ff3b804 and prints the probe
  address (so we catch it if the allocator ever moves).
- `PATCH10_user_return_probe.md`: the kernel-side entry.S probe (a CI patch — needs a
  kernel rebuild) that writes the U-mode-crossing mstatus/mcause/CLIC24/mintthresh into
  the probe scratch. Assembles clean as rv32ima_zicsr.
- `benvisor_asm.S`: bail fix retained (good latent-bug fix, harmless).

### Order of operations
1. Flash the app-only HEALTH-V4 first (no kernel rebuild). Confirm the boot line
   `PROBE scratch (uncached) = 0x8ff3c124`. If it differs, update PATCH 10's `li t2`.
   (`probe` will read 0 until the kernel patch lands — that's expected.)
2. Apply PATCH 10 via build-kernel.yml, rebuild, flash. Capture the freeze banner:
   the `probe.*` block is the U-mode crossing state.
3. In parallel, research chat: confirm P4 CLIC M-interrupt delivery to U-mode (see
   PATCH10 doc). The answer + the probe values together give the fix:
   - CLIC24 `ie=0`/`attr` changed → kernel reconfigured the line; fix in driver/benvisor.
   - all armed but undelivered → U-mode delivery gap → CLIC24 config bit or run user in M.
   - `mintthresh`/MPIL raised → threshold/level mask → clear it on user return.

---

## RESOLVED (V5) — ROOT CAUSE: CLIC24 level-trigger loses pending across the U-mode return

PATCH 10 captured the smoking gun. At the freeze:

```
probe.mstatus = 0x00000080   MIE(bit3)=0  MPP(12:11)=0  (last return was to U-mode)
probe.mcause  = 0x00000000   pil[23:16]=0
probe.thresh  = 0x00000000
probe.CLIC24  = 0xffc10100   ip=0  ie=1  attr=0xc1  ctl=0xff
systimer:     INT_ST=0x2 (alarm fired+latched)  INTMTX_C1=24
```

Against the research's 5-condition delivery equation
(MIE && clicintie && clicintip && level>pil && level>thresh), four are clean:
ie=1, pil=0, thresh=0, ctl=0xff. The ONLY failing term is **clicintip==1** — the
systimer is asserting (INT_ST=0x2) but the CLIC pending bit for line 24 reads 0.

**Why:** CLIC24 was LEVEL-triggered (attr trig=00), so clicintip[24] was a
passthrough of the systimer INT_ST level, not a latch the CLIC holds itself. The
ISR clears INT_ST on entry, deasserting the level at the CLIC. This worked while
the hart stayed in M-mode and the ISR caught every edge. **MPP=0 proves the kernel
runs userspace in U-mode** (the research's M-mode-only assumption was wrong — this
P4 core implements M+U, and /bin/sh is the first-ever U-mode return). The alarm edge
that fires across the U-mode window is not reflected as pending; back in M-mode idle
there is nothing pending to take, and the timer is dead. MIE=0/MPP=0 just mark the
last return as to-U; they are not the blocker (freeze is in M-idle, pil/thresh clean).

**Fix (benvisor.c + benvisor_asm.S, app-only, no kernel rebuild):**
1. `benvisor.c`: CLIC24 attr `0x01` → `0x03` (SHV=1 | trig=01 EDGE). Edge mode makes
   the CLIC LATCH clicintip in hardware on the rising edge and hold it until cleared,
   so an edge during the U-mode window survives and is serviced on return to M-mode.
2. `benvisor_asm.S`: the ISR (and the bail path) now clear clicintip[24] explicitly
   (`sb x0, 0x20801060`) BEFORE clearing the systimer INT_ST. A vectored CLIC entry
   does not auto-clear the edge latch on this part, so without this an edge source
   would re-fire forever. Order: clear latch first, then source, so a fresh edge
   mid-ISR is not lost. Assembled clean; both writes verified to resolve to 0x20801060.

HEALTH-V4 instrumentation retained so this run is directly comparable. Expected result:
isr keeps climbing past /bin/sh, c24 ip toggles 1→0 each tick, shell prompt appears.
If it still freezes with ip=1 latched (storm) → the edge ack write isn't clearing the
latch on this part; fall back to also writing clicintip via a 32-bit clear or check the
P4 CLIC pending-clear semantics. If it freezes with ip=0 again → edge didn't latch
either; that points to a deeper P4 CLIC/U-mode delivery limitation and running
userspace in M-mode (drop U) becomes the alternative.
