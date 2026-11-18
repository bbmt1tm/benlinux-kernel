# Test 1 — Drop the exec mapping

## What changed

**One change only**, isolated to `main/main.c` → `load_kernel_and_dtb()`:

- Removed the `esp_mmu_map(MMU_MEM_CAP_EXEC | ... , ESP_MMU_MMAP_FLAG_PADDR_SHARED, ...)` call.
- Removed the `0x4A000000`-based `exec_base` / `exec_vaddr` variables.
- DTB is now placed and read at the same address: `psram_base + 15 MB = 0x48F00000`.
- Kernel entry is now `psram_base = 0x48200000` (linker base = runtime base).
- Added a kernel-image magic check (RISC-V Image header at offset `0x38` = `0x05435352`).

**Nothing else changed.** `sdkconfig` is untouched. No new files. No deleted files. `benvisor.c`, `benvisor_asm.S`, `esp32p4.dts`, `esp32p4_dtb.h`, kernel build patches, IPC layout, Core 1 task flow, WFI patch, alarm rearming — all identical to your master.

## Why

Per the research report (§4, §5), on ESP32-P4 with `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` and `CONFIG_SOC_MMU_DI_VADDR_SHARED=y`, the IDF linker script declares `extern_ram_seg(RWX) : org = 0x48000000, len = 64MB`. The whole PSRAM region at `0x48000000+` is already dual-bus (instruction + data) executable through a single virtual address window. The previous `esp_mmu_map(EXEC, PADDR_SHARED)` call created a **second** virtual mapping at `0x4A000000` that was instruction-bus-only and incoherent with the data-bus writes used to load the kernel image. Predicted consequences (which match the observed "K then silence"):

1. Kernel linked at `0x48200000` but PC-started at `0x4A000000`. All `la` resolves into `0x4A...` (kernel built with `-mcmodel=medany`, every symbol is PC-relative).
2. NOMMU `__pa(x) = x`. `setup_bootmem` calls `memblock_reserve(__pa(_start), kernel_size)` with `_start = 0x4A000000`, but the DTS-declared memory range is `[0x48200000, 0x49C00000)`. The reservation is outside the range and silently no-ops.
3. First `memblock_alloc` returns PSRAM pages from `[0x48200000, 0x49C00000)` that physically overlap the running kernel. Writes via those addresses corrupt the kernel's own code/data.
4. The kernel makes it as far as registering earlycon and emitting the first byte of `"Kernel command line: ..."` ("K"), then dies before the second byte.

Eliminating the second mapping removes all of that. The kernel runs at its link address; `__pa()` returns addresses inside the DTS memory range; `memblock_reserve` works; the kernel image is protected.

## What to look for in the serial output

The new log lines you should see (Core 0 side, before Core 1 launches):

```
I (...) benlinux: Cache flush: OK
I (...) benlinux: DTB patched: IPC base = 0x800Fxxxx        <-- uncached SRAM alias
I (...) benlinux: DTB at 0x48f00000 (1136 bytes)            <-- now in 0x48F... not 0x4AF...
I (...) benlinux: Kernel header check: first_insn=0x........ magic2@+0x38=0x05435352 (OK)
```

The key diagnostic is `magic2@+0x38=0x05435352 (OK)`. If you see that, the loaded image is intact when read through `0x48200000`, which is the same virtual address the kernel will execute from. That's the precondition the previous design violated.

The pre-boot verification block in `core1_linux_task` (around line ~462) will now show:

```
[CORE1] Kernel @ 0x48200000: first insn = 0x........
[CORE1] DTB @ 0x48f00000: VALID                              <-- not 0x4af00000
```

## Three possible outcomes, and what they mean

### Outcome A — kernel boots significantly further
Expected sequence after `[CORE1] Jumping to kernel...`:
```
K                                                            <-- (already had this)
ernel command line: <args>
[    0.000000] Linux version 6.12...
[    0.000000] Machine model: ...
...
[    0.000000] earlycon: ... at MMIO 0x800F0920 (options '')
[    0.000000] printk: legacy bootconsole [...] enabled
[    0.000000] Built 1 zonelists ...
...
+                                                            <-- BenVisor timer heartbeat
```
This confirms the exec-mapping aliasing was the dominant failure. Whatever happens after is the **next** problem to diagnose.

### Outcome B — same symptom (one "K", then silence)
Means the exec mapping was not the dominant failure. Most likely culprits, in order:
1. `mtvec` not being written by BenVisor (relies on FreeRTOS leftover state — see report §7).
2. Cache coherency between Core 0 (which loads and patches) and Core 1 (which executes). Even with the fence.i, if Core 1's I-cache was pre-warmed by some other code path, stale lines may persist. **Next test** would be to add a Core-1-side cache invalidate (`esp_rom_Cache_Invalidate_ICache_All` or equivalent) before the kernel jump.
3. `CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y` may have set PMP/PMA entries that prevent execution from `0x48200000` if it falls outside the linker-defined `irom_seg`. This would manifest as an instruction access fault. **Next test** would be to read back the PMP entries on Core 1 and verify `0x48200000` is RX.

### Outcome C — earlier hang (no "K" at all)
Means we broke something we shouldn't have. Roll back to original `main.c` (kept in your master), and we'll re-examine what changed.

## Next-step preparation (do not apply yet)

If Outcome A, the next thing to chase will likely be the Core 1 timer ISR (your Session 30 blocker). Don't change anything for that yet — let's see how far the kernel gets first.

If Outcome B, I have two ready follow-up tests in mind:
- **Test 2a**: add `esp_rom_Cache_Invalidate_ICache_All()` on Core 1 immediately before `benvisor_boot_linux`.
- **Test 2b**: read back PMP cfg/addr on Core 1 around the kernel jump and print whether `0x48200000` is RX-permitted.

Pick whichever makes sense based on serial output.

## Files in this drop

```
benlinux_project_test1/
├── CMakeLists.txt          (unchanged from master)
├── README.md               (unchanged)
├── partitions.csv          (unchanged)
├── sdkconfig               (UNCHANGED — do not regenerate)
├── sdkconfig.defaults      (unchanged)
├── components/
│   └── benvisor/
│       ├── include/
│       │   ├── benvisor.h          (unchanged)
│       │   └── esp32p4_dtb.h       (unchanged)
│       └── src/
│           ├── benvisor.c          (unchanged)
│           ├── benvisor_asm.S      (unchanged)
│           └── esp32p4.dts         (unchanged)
└── main/
    ├── CMakeLists.txt              (unchanged)
    └── main.c                      (CHANGED — load_kernel_and_dtb() only)
```

Build: `idf.py build flash monitor` as usual. Same kernel image on SD card as before.
