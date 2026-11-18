# Test 2 — PMP permissive override on Core 1

## What Test 1 told us

```
[CORE1] Kernel @ 0x48200000: first insn = 0x0600006f       <-- valid jal, image loaded fine
[CORE1] DTB @ 0x49100000: VALID
[CORE1] Patched 3 WFI → NOP                                  <-- D-bus R+W on 0x48200000 works
[CORE1] Jumping to kernel...
Guru Meditation Error: Core 1 panic'ed (Instruction access fault)
MEPC=0x48200000  MCAUSE=0x1  MTVAL=0x48200000  MSTATUS=0x00011800
```

- D-bus R+W on `0x48200000`: OK
- I-bus fetch from `0x48200000`: FAULT
- Core in M-mode (`MSTATUS.MPP = 11`)
- Fault on the very first byte (`MEPC = MTVAL = kernel entry`)

This asymmetry — R/W allowed, X denied, in M-mode — is the **PMP signature** for the case where `CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y` set up entries that grant R/W to the heap portion of PSRAM but not X. In M-mode, PMP only restricts if entries have `L=1` locked. So IDF locked the protection entries.

Test 1 successfully ruled out cache aliasing, BSS clobber, and link-vs-runtime address mismatch — `magic2@+0x38=0x05435352 (OK)` confirmed the image is intact at `0x48200000`. The only remaining barrier is permission, not addressing.

## What changed in Test 2

**One change**, in `main/main.c` → `core1_linux_task`, inserted between "Pre-boot verification" and "Prepare for kernel jump":

1. **Read and print all 16 PMP entries** (`pmpcfg0-3`, `pmpaddr0-15`). Decodes each into R/W/X flags, address mode (OFF/TOR/NA4/NAPOT), lock bit, and base address.

2. **Write a permissive PMP entry 0**: NAPOT covering `[0x40000000, 0x80000000)` with RWX, no lock. This is the same trick Linux's `head.S` uses at `.Lpmp_done` (`pmpaddr0=-1, pmpcfg0=0x1F`) — we just do it slightly earlier so we can *reach* `head.S`. Read-modify-write preserves entries 1–3 (locked bytes ignored by the write per RISC-V spec).

3. **Readback verification**: prints `TOOK` if the override is in effect, or `WRITE-IGNORED — entry 0 was locked` if IDF locked it.

4. **Fallback**: if entry 0 is locked, walks entries 1–15 to find the first unlocked slot and writes a permissive NAPOT entry there. Caveat printed: lower-index entries take precedence, so a fallback only helps if no lower entry already matches `0x48200000` with X=0.

Nothing else changed. `sdkconfig` untouched. `benvisor.c`, `benvisor_asm.S`, DTS, DTB, kernel patches, `load_kernel_and_dtb()` (Test 1's change) — all carried forward identically.

## What to look for in the serial output

Before `[CORE1] Alarm re-armed, jumping with MIE=0`, you'll now see:

```
[DIAG] === PMP state before override ===
[DIAG] pmpcfg0=0x........ 1=0x........ 2=0x........ 3=0x........
[DIAG] PMP[ 0]: cfg=0x.. [RWX NAPOT L=?] addr=0x........
[DIAG] PMP[ 1]: cfg=0x.. [...] addr=0x........
...
[DIAG] PMP[15]: cfg=0x.. [...] addr=0x........
[CORE1] Writing permissive PMP entry 0 (NAPOT 0x40000000-0x80000000 RWX)
[CORE1] Readback: pmpcfg0=0x........ pmpaddr0=0x17ffffff (TOOK | WRITE-IGNORED ...)
```

The key lines I need from you:

- **The full PMP dump** — this tells us exactly what IDF set up. Look for any entry whose address range covers `0x48200000-0x49C00000` and check its flags. If you see an entry like `cfg=0x83 [RW- NAPOT L=1] addr=0x48000000` (L=1, no X, covering that range), that's the smoking gun.

- **The readback line** — `TOOK` means our override is live; `WRITE-IGNORED` means IDF locked entry 0.

## Three possible outcomes

### Outcome A — `TOOK`, kernel boots significantly further
PMP was the entire issue. Expect to see:
```
[CORE1] Readback: ... (TOOK)
[CORE1] Alarm re-armed, jumping with MIE=0
[CORE1] Patched 3 WFI → NOP
[CORE1] Jumping to kernel...
K
ernel command line: <args>
[    0.000000] Linux version 6.12...
```
Whatever happens next is the new problem.

### Outcome B — `TOOK`, but still Instruction Access Fault
Means the PMP override IS in effect (priority-0 entry RWX matching `0x48200000`) but the fault persists. That rules out PMP and points to **cache/bus** as the culprit:
- ESP32-P4 L1 I-cache may not have a bus mapping for the PSRAM heap region on Core 1
- IDF's `cache_ll_l1_enable_bus(1, CACHE_BUS_IBUS, ...)` may not have been called for Core 1
- Or the cache MMU table only has D-bus translations for `0x48200000`

If this happens, Test 3 will need to either (a) poke the cache config registers directly or (b) call `esp_mmu_map(EXEC, PADDR_SHARED)` to create the I-bus mapping at a *different* vaddr and patch the kernel to use that as `CONFIG_PAGE_OFFSET`. We'd cross that bridge then.

### Outcome C — `WRITE-IGNORED` (entry 0 locked)
The fallback loop tries slots 1–15. If any unlocked slot was written, the kernel jump still has a chance. But since PMP is "first match wins" and lower indices win, if entry 0 is locked AND matches `0x48200000` AND lacks X, the fallback won't help.

If you see this AND the kernel still faults, Test 3 will need to disable `CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION` in sdkconfig (edit one line — not a regenerate). This is the cleanest escape hatch but the dirtiest from a "minimal config diff" perspective.

## What I'm betting on

I think `TOOK` and Outcome A. ESP-IDF on ESP32-P4 generally doesn't lock its protection entries (`L=0` is the default in `cpu_region_protect.c`), so the M-mode override should take effect. If it doesn't, we have a clear next step.

## Files in this drop

Same structure as Test 1:
```
test2_pmp_fix/
├── CMakeLists.txt           (unchanged)
├── README.md                (unchanged)
├── partitions.csv           (unchanged)
├── sdkconfig                (UNCHANGED)
├── sdkconfig.defaults       (unchanged)
├── components/              (unchanged)
└── main/
    └── main.c               (CHANGED — Test 1 carryover + new PMP block)
```

Only `main/main.c` differs from your master. Test 1's `load_kernel_and_dtb()` simplification is preserved.

Build & flash as usual: `idf.py build flash monitor`.
