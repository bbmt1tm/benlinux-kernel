# Test 6 — D-bus write coherency probe from Core 1

## What Test 5 told us (huge finding)

The forensic scan found **17 matches** in PSRAM — all of them are `.rodata` format strings from the kernel binary (e.g., `"Kernel command line: %s\n"`) or the bootargs from inside the DTB. **Zero formatted `printk` log entries.**

```
[FORENSIC] FOUND "Kernel command line:" at 0x48660c3e
[FORENSIC]   "Kernel command line: %s\n"                  ← format string, not output
[FORENSIC] FOUND "Linux version " at 0x485d9e64
[FORENSIC]   "Linux version 6.12.71 (runner@runnervmeorf1) ..."   ← linux_banner constant in .rodata
```

If `printk` had actually run, we'd see entries like `"Linux version 6.12.71 ..."` *without* the `%s` placeholder, *with* contextual data after the format substitution. We see only the raw format strings → `printk` never executed.

Plus: `IPC head/tail = 0` → the earlycon driver never wrote to the IPC ring either.

**Conclusion: the kernel never reached `start_kernel`.** It died somewhere inside `head.S`, between the jump landing at `0x4A000000` and the call to `_start_kernel`.

The "K" we see in the UART output is *not* from kernel `printk`. It's something else — possibly UART noise during the jump transition, a stale FIFO byte, or one byte from a different code path.

## The most likely failure mode

`head.S` does three big operations before calling `_start_kernel`:

1. PMP setup (`.Lpmp_done` writes `pmpcfg0=0x1F` — silently ignored, locked, no fault)
2. `reset_regs` (zeros all registers including `sp` — OK with `HW_STACK_GUARD=n`)
3. **BSS clear** (~21 MB of zeroing via PC-relative `la __bss_start; la __bss_stop; loop store zero`)
4. Stack setup, then `call _start_kernel`

Step 3 is the prime suspect. The kernel writes zeros via PC-relative addresses that resolve to `0x4Axxxxxx` (the exec alias). **If those writes don't actually persist to physical PSRAM, the kernel's BSS isn't really being cleared, AND any subsequent write to `.data` or `log_buf` vanishes too** — explaining why we found zero `printk` entries.

The WFI patch in Test 5 "found 3 WFI" and "patched" them — but we never verified the writes persisted. The counter just records *attempts*, not successes. If Core 1's D-bus writes to `0x4A...` go nowhere, the WFI bytes are still there at I-fetch time.

## What Test 6 does

**One additive change in `main/main.c` → `core1_linux_task`**, inserted right before the WFI patch block.

### Probe 1: D-bus write coherency

Pick three offsets well past the kernel image (in the BSS region where head.S would be writing zeros):
- `+0x800000`, `+0x A00000`, `+0xC00000` (8MB, 10MB, 12MB into the kernel area)

For each:
1. Read existing value via exec alias (`0x4A...`) — `before_exec`
2. Read existing value via data alias (`0x48...`) — `before_data`
3. Write known pattern via exec alias (`0x4A...`)
4. Flush D-cache, invalidate via opposite alias
5. Read back via exec alias — `after_exec`
6. Read back via data alias — `after_data`

Then classify:
| Outcome | Diagnosis |
|---|---|
| Both readbacks see pattern | **COHERENT** — D-bus writes via 0x4A persist as expected. Failure must be elsewhere in `head.S`. |
| Only exec alias sees pattern | **EXEC-only** — written to a cache that the data alias can't see. Coherency broken between alias views. |
| Only data alias sees pattern | **DATA-only** — odd; suggests cache write was redirected, exec read is stale. |
| Neither sees pattern | **FAIL** — D-bus write was dropped entirely. Kernel BSS clear is no-op. |

### Probe 2: Reverse direction

Also tests the reverse: write via `0x48` (data alias), read back via `0x4A` (exec alias). If forward works but reverse doesn't, the kernel can't observe Core 0's earlier writes correctly.

### Probe 3: WFI patch verification

After the existing WFI patch block, **rescan to count remaining WFIs**. If patch worked: 0 remaining. If writes vanished: still 3.

## Three outcomes I'm watching for

### Outcome A — All probes show "COHERENT" + 0 WFI remaining
D-bus writes via `0x4A` work correctly. The kernel's BSS clear is fine. The failure inside `head.S` must be elsewhere. Test 7 would be more invasive — probably modifying the kernel image at runtime to write a known byte to UART at specific offsets in `head.S` to trace where it dies.

### Outcome B — "FAIL" or "EXEC-only" + WFI patch didn't take
D-bus writes via `0x4A...` are broken. Confirms diagnosis. Fix would be to either:
- Create a second `esp_mmu_map(..., R|W, PADDR_SHARED)` mapping at the same vaddr — but the IDF API may not allow same vaddr in two calls
- Manually program the cache-MMU registers to add data-side mappings
- OR put the kernel somewhere else entirely (e.g., into IRAM via a load-and-jump trampoline, if it fits — won't, it's 5MB)
- OR build the kernel at a different link base such that the write address resolves into a region that IS coherent

### Outcome C — Probes succeed but WFI still 3 remaining
Means D-bus writes work for unrelated addresses but the kernel image region specifically is in a read-only mapping. PMP[7] (`R-X TOR @ 0x4802E980`) covers `[0x48000000, 0x4802E980)` as R-X only — but our probes are far above that. Still worth checking.

## Files in this drop

```
test6_dbus_test/
├── CMakeLists.txt          (unchanged)
├── README.md               (unchanged)
├── partitions.csv          (unchanged)
├── sdkconfig               (UNCHANGED)
├── sdkconfig.defaults      (unchanged)
├── components/benvisor/
│   ├── include/
│   │   ├── benvisor.h      (unchanged)
│   │   └── esp32p4_dtb.h   (Test 4 — memory@4a000000)
│   └── src/
│       ├── benvisor.c      (unchanged)
│       ├── benvisor_asm.S  (unchanged)
│       └── esp32p4.dts     (Test 4 — memory@4a000000)
└── main/
    └── main.c              (Test 5 + D-bus probe + WFI verify)
```

Flash. The `[DBUS]` lines and `[CORE1] WFI verify: ...` line are what to look for. They'll tell us conclusively whether Core 1's writes to `0x4A...` actually persist.
