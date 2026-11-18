# Test 3 — Restore exec mapping (revert Test 1) + keep diagnostics

## What we learned from Tests 1 & 2

**Test 1 was a regression.** I removed the `esp_mmu_map(EXEC)` call based on my research saying the aliasing scheme had fatal `memblock` problems. The actual project history (your BenOS-hosted run reaching "kernel boot messages and console open") proves the aliasing is tolerable in practice — the kernel survives it well enough to make significant progress.

**Test 2 confirmed PMP layout.** The dump showed exactly what your `cpu_region_protect.c` says it does:

```
PMP[ 6]: --- OFF L=1 @ 0x48000000            (boundary)
PMP[ 7]: R-X TOR L=1 @ 0x4802E780            (.flash.text)
PMP[ 8]: RW- TOR L=1 @ 0x48030000            (padding)
PMP[ 9]: R-- TOR L=1 @ 0x4803F180            (.flash.rodata)
PMP[10]: RW- TOR L=1 @ 0x4A000000            (PSRAM heap — NO X)
PMP[11]: --- OFF L=1 @ 0x50108000            (next: LP memory)
```

The critical insight: **`[0x4A000000, 0x50000000)` is not covered by any PMP entry.** In M-mode, unmatched addresses are permitted regardless of lock state (RISC-V Priv spec § 3.7). That's how the BenOS-hosted kernel ran from `0x4A000000+` — it was simply outside the protected ranges.

The `cpu_region_protect.c` file you uploaded (current and "older backup") are **md5-identical** — same `4a7ce2270d4f177b3afd58e80e4975d0`. Same file that was active in BenOS-hosted. The PMP setup is not the problem; my approach in Test 1 was.

## What changed in Test 3

Started from **your master** (so the exec mapping is back exactly as it was). Two additive diagnostics:

1. **Dual-alias image-magic check** in `load_kernel_and_dtb`:
   ```
   I (...) benlinux: Kernel header via DATA  alias 0x48200000: insn=0x........ magic2=0x........ (OK|BAD)
   I (...) benlinux: Kernel header via EXEC  alias 0x4a000000: insn=0x........ magic2=0x........ (OK|BAD)
   ```
   If both report `OK` and the bytes match, we've confirmed that writes via the data alias are visible through the exec alias at this exact moment. If they disagree, that's our cache-aliasing smoking gun.

2. **PMP snapshot** in `core1_linux_task`, just before the kernel jump:
   - Same 16-entry dump as Test 2, no write attempt
   - Plus an explicit check that `kernel_entry` is in `[0x4A000000, 0x50000000)` (the unmatched range)

Nothing else changed. `load_kernel_and_dtb()` is master's exec-mapping version. `sdkconfig`, `benvisor.c`, `benvisor_asm.S`, DTS, DTB, Core 1 task flow — all carried forward identically.

## What to look for in the output

```
I (...) benlinux: PSRAM physical addr: 0x00200000
I (...) benlinux: Executable mapping: paddr=0x00200000 -> vaddr=0x4a000000 (...)
I (...) benlinux: DTB at 0x4af00000 (1136 bytes)
I (...) benlinux: Kernel header via DATA  alias 0x48200000: insn=0x0600006f magic2=0x05435352 (OK)
I (...) benlinux: Kernel header via EXEC  alias 0x4a000000: insn=0x0600006f magic2=0x05435352 (OK)
...
[CORE1] Kernel @ 0x4a000000: first insn = 0x0600006f
[CORE1] DTB @ 0x4af00000: VALID
[DIAG] === PMP snapshot ===
[DIAG] PMP[ 0]: ...
...
[CORE1] Kernel entry 0x4a000000 is in unmatched PMP range [0x4A000000, 0x50000000) — execute should be permitted
[CORE1] Alarm re-armed, jumping with MIE=0
[CORE1] Patched 3 WFI → NOP
[CORE1] Jumping to kernel...
K            <-- if "K then silence" → we're back to original baseline
ernel command line: ...   <-- if more text → we got further than original
```

## What this test tells us

There are now three meaningful outcomes:

### Outcome A — kernel boots significantly past "K"
Means: my Test 1 was simply wrong, master + better diagnostics is fine, and we're now into whatever blocked the previous BenOS-hosted run from progressing further (likely the Session-30 timer-ISR-stops-firing issue).

### Outcome B — "K then silence" (same as original master)
Means: we're at the original baseline. Now the question is *why does it die after one byte*. Hypotheses to test in order:
1. **DT memory range mismatch**: DTS says `memory@48200000` but kernel runs at `0x4A000000`. `__pa(_start)` = `0x4A000000` outside DT range → `memblock_reserve` no-op → kernel image not protected from `memblock_alloc`. Fix = update DTS to `memory@4a000000 reg = <0x4a000000 0x01a00000>` and regenerate `esp32p4_dtb.h`. (Test 4a)
2. **D-side write incoherence at `0x4A...`**: even with `PADDR_SHARED + DI_VADDR_SHARED`, kernel stores to PC-relative `0x4A...` addresses (BSS clear, stack writes) might not reach physical PSRAM if no D-bus mapping. Fix = ensure D-bus mapping at `0x4A...` either by additional `esp_mmu_map(R|W, PADDR_SHARED)` or by adjusting `CONFIG_SOC_MMU_DI_VADDR_SHARED` expectations. (Test 4b)
3. **earlycon driver mismatch**: the patched `benvisor-console.c` may have a stale IPC offset. Worth verifying `IPC_ROM_PUTC_ADDR_OFF = 0x91C` (2332) matches the kernel patch literal.

I'd test these in order — DTS is the cheapest and most likely.

### Outcome C — earlier hang or fault before "K"
Means something else changed in the environment between today and the original "K then silence" run. PMP snapshot tells us if the layout drifted. Unlikely but possible.

## Files in this drop

```
test3_restore_exec/
├── CMakeLists.txt          (master, unchanged)
├── README.md               (master, unchanged)
├── partitions.csv          (master, unchanged)
├── sdkconfig               (UNCHANGED)
├── sdkconfig.defaults      (master, unchanged)
├── components/             (master, unchanged)
└── main/
    └── main.c              (master + dual-alias magic check + PMP snapshot)
```

Build & flash as usual. Looking forward to the PMP snapshot + whatever the kernel says.
