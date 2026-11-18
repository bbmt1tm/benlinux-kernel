# Test 4 — Update DT `memory@` node to `0x4A000000`

## What Test 3 told us

```
[CORE1] Kernel @ 0x4a000000: first insn = 0x0600006f                              ← jumped cleanly
[CORE1] DTB @ 0x4af00000: VALID
[DIAG] PMP[10]: cfg=0x8b [RW- TOR L=1] addr=0x4a000000                            ← end of PSRAM heap range
[CORE1] Kernel entry 0x4a000000 is in unmatched PMP range — execute permitted    ← confirmed
[CORE1] Jumping to kernel...
K                                                                                  ← earlycon worked once
[HEALTH] tgt=0x0573d12f(STA) ST=0x2 ENA=0x3 CONF=0xff800002                       ← alarm pending, never serviced
```

Key facts:
- Kernel jumped successfully — no fault
- Dual-alias check: both `0x48200000` and `0x4A000000` read identical kernel header bytes — PSRAM aliasing is coherent
- "K" emitted = kernel reached `start_kernel`, registered earlycon, and started the first `printk("Kernel command line: ...")`
- Then silence — kernel died between byte 1 ('K') and byte 2 ('e')
- HEALTH lines: alarm pending forever, `MIE = 0` still → kernel never reached `local_irq_enable()`

## The diagnosis

This is `memblock` self-corruption, just as my original research predicted (and as I temporarily talked myself out of after seeing BenOS-hosted reached further). Here's the chain:

1. Kernel runs at `0x4A000000` (from exec mapping)
2. DTS declares `memory@48200000 reg = <0x48200000 0x01A00000>` → memblock memory range = `[0x48200000, 0x49C00000)`
3. In NOMMU, `__pa(x) == x`. So `__pa(_start) = 0x4A000000`
4. `setup_bootmem` calls `memblock_reserve(0x4A000000, ~5MB)` — **outside the memory range**, silently no-op'd (added to the reserved list but doesn't protect anything inside `[0x48200000, 0x49C00000)`)
5. First `memblock_alloc(...)` returns a page from `[0x48200000, 0x49C00000)`, say `0x48300000`
6. Vaddr `0x48300000` (data alias) and `0x4A100000` (exec alias) map to the **same physical PSRAM**
7. Kernel writes early-boot data via `0x48300000` → physically overwrites kernel code at `0x4A100000`
8. Next instruction fetch from corrupted code → kernel crashes with `MIE = 0`, silent death

The reason this didn't bite in BenOS-hosted as severely is probably that BenOS had a slightly different boot path that delayed the first conflict-causing `memblock_alloc`, or wrote it to a region that happened not to overlap the kernel image. Same fundamental bug, different symptom severity.

## What changed in Test 4

**Two coordinated changes** (kernel-side and DTB-side; they go together):

1. **`components/benvisor/src/esp32p4.dts`** — memory node updated:
   ```
   memory@4a000000 {
       device_type = "memory";
       reg = <0x4A000000 0x01A00000>;   /* was 0x48200000 */
   };
   ```

2. **`components/benvisor/include/esp32p4_dtb.h`** — regenerated from the new DTS via `dtc`. New DTB is 1140 bytes (was 1136 — the slight increase is from the "BenLinux Standalone ESP32-P4" model string which is now 1 char longer than the old "BenOS ESP32-P4 Native Linux" string that was in the stale embedded blob). `ESP32P4_DTB_SIZE` macro updated to 1140.

   Verified:
   - `0xCAFEBABE` console placeholder still present at offset 924 → runtime IPC patch will still work
   - `0x4A000000` bytes present in memory@ node
   - `0x48200000` bytes are GONE — no stale references

3. **`main/main.c`** — unchanged from Test 3 (master's exec-mapping code + dual-alias magic check + PMP read-only dump).

## Memory layout now consistent

| Address | Meaning | Phys PSRAM offset |
|---|---|---|
| `0x4A000000` | Kernel image start (entry point, exec alias) | `0x200000` |
| `0x4A4BE2B4` | Kernel image end (~5MB) | `0x6BE2B4` |
| `0x4AF00000` | DTB | `0x1100000` |
| `0x4BA00000` | End of DT-declared memory range (26MB) | `0x1C00000` |
| `0x48200000` | Same kernel image bytes, data alias (used to load image) | `0x200000` |

The kernel's view:
- `memblock_add(0x4A000000, 0x01A00000)` → memory `[0x4A000000, 0x4BA00000)`
- `memblock_reserve(__pa(_start) = 0x4A000000, ~5MB)` → kernel image protected ✓
- `memblock_alloc` returns from `[0x4A4BE2B4, 0x4BA00000)` (excluding reserved DTB region) → maps to physical `[0x6BE2B4, 0x1C00000)`, no overlap with kernel image ✓

## What to look for in serial output

Same Test 3 boot sequence up to the kernel jump. Then:

### Outcome A — kernel boots significantly past 'K'
Expected sequence:
```
[CORE1] Jumping to kernel...
Kernel command line: earlycon console=ttyBV0 lpj=3600000 root=/dev/ram0 nosoftlockup nohz=off rdinit=/bin/sh
[    0.000000] Linux version 6.12...
[    0.000000] Machine model: BenLinux Standalone ESP32-P4
[    0.000000] earlycon: ... at MMIO 0x800F0920 (options '')
[    0.000000] printk: legacy bootconsole [...] enabled
[    0.000000] Built 1 zonelists in Normal zone ...
[    0.000000] Kernel command line: ...
[    0.000000] Memory: ...
...
+                                                            ← timer ISR heartbeat (BenVisor)
```

This is what we want. Whatever happens next is the new problem (probably the Session 30 timer-ISR-stops-firing).

### Outcome B — same "K then silence"
Means memblock wasn't the issue, or there's another problem at the same stage. Hypotheses left to test:
- **D-side write incoherence at `0x4A...`**: even with PADDR_SHARED, kernel stores via PC-relative `0x4A...` addresses may not reach physical PSRAM. Would need to verify by adding a Core 1 readback loop that polls a known kernel BSS location to see if writes are coherent.
- **earlycon driver mismatch**: the kernel patch in `build-kernel.yml` may have a stale IPC offset literal. Worth grep'ing the patch for `0x91C` or `2332`.
- **DTB IPC patch missing the new DTB layout**: the runtime CAFEBABE search-and-replace might be at a different offset in the new DTB. The diagnostic confirmed CAFEBABE is at offset 924 in the new DTB — should still be found by the existing search loop in `load_kernel_and_dtb`.

### Outcome C — fault at a different address than `0x4A000000`
Means the kernel got further (good) but died from a permission or access fault. The MEPC/MTVAL in the dump will tell us where. Likely a write fault at some `0x4A...` address if D-bus writes aren't coherent.

## Files in this drop

```
test4_memory_at_4a/
├── CMakeLists.txt                  (unchanged)
├── README.md                       (unchanged)
├── partitions.csv                  (unchanged)
├── sdkconfig                       (UNCHANGED)
├── sdkconfig.defaults              (unchanged)
├── components/benvisor/
│   ├── include/
│   │   ├── benvisor.h              (unchanged)
│   │   └── esp32p4_dtb.h           (REGENERATED — new memory@ + size 1140)
│   └── src/
│       ├── benvisor.c              (unchanged)
│       ├── benvisor_asm.S          (unchanged)
│       └── esp32p4.dts             (CHANGED — memory@4a000000)
└── main/
    └── main.c                      (Test 3 state — exec mapping + diagnostics)
```

Three files differ from master. `sdkconfig` is untouched.

Build & flash. The PMP snapshot will look the same as Test 3 (no environmental change there). The DTB at `0x4AF00000` will still report VALID. The interesting question is whether the kernel makes it past 'K' this time.
