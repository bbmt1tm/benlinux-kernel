# Test 5 — Forensic PSRAM scan after kernel dies

## What Test 4 told us

Same baseline as Test 3: kernel jumps cleanly, emits 'K', goes silent with `MIE=0` (alarm pending, never serviced). The DTB memory range fix didn't change the symptom. Either `memblock` self-corruption wasn't the issue, or there's another problem at the same boot stage.

## The forensic approach

Rather than continue guessing, this test reads the actual kernel state from PSRAM after the kernel has died. The trick: **printk writes to its internal `log_buf` BEFORE it tries to flush to console**. So even if only 'K' came out the UART, the *full* formatted "Kernel command line: ..." string should be sitting in `log_buf` somewhere in PSRAM. Same for any earlier `printk` messages.

Core 0 will (after ~3 seconds, once Core 1 is presumably wedged) scan the 26 MB kernel region in PSRAM looking for known kernel boot log strings:

1. `"Linux version "` — first `pr_notice` in `start_kernel` (before "Kernel command line")
2. `"Kernel command line:"` — what we're seeing 'K' from
3. `"earlycon"` — appears in earlycon registration log line
4. `"Machine model:"` — from DTB parsing
5. `"Built 1 zonelists"` — much later in `start_kernel`, after `setup_arch`

The scan is via the data alias `0x48xxxxxx` (same physical PSRAM as kernel uses at `0x4Axxxxxx` via exec alias). No interference with Core 1.

Each match prints up to 120 printable bytes of context, stopping at `\0` or newline. So we'll see the actual formatted log messages, with timestamps and content.

## What each outcome tells us

### Outcome A — all 5 markers found
Kernel ran very far through `start_kernel`. The earlycon driver itself broke after first byte, but kernel kept executing. We'd find log messages well past "Kernel command line" — `setup_arch`, memory init, etc. Fix would be in the earlycon driver patch.

### Outcome B — first 3 found, not "Built 1 zonelists"
Kernel reached and printed "Kernel command line" via printk (it's in log_buf) but died somewhere in `setup_arch` or `parse_early_param`. Most likely cause: memblock initialization actually hit a corruption issue *after* the bootmem reserve (despite Test 4's DT fix).

### Outcome C — "Linux version" found, "Kernel command line" NOT
Kernel died *between* the two `pr_notice` calls. Something in `setup_arch`, `setup_command_line`, `setup_nr_cpu_ids`, `smp_prepare_boot_cpu`, or `boot_cpu_hotplug_init` (the calls between them in `start_kernel`).

### Outcome D — nothing found
Kernel died before reaching any `pr_notice`. Earlycon must have been registered via a different path — the bootargs parser maybe? Or the 'K' we saw was from somewhere else entirely (kernel banner format string in `.rodata` shouldn't print, but...).

### Outcome E — fragments of strings but corrupted
Kernel's log_buf got overwritten with bogus data. Self-corruption confirmed. The first-64-bytes dump of kernel image will tell us if the entry code was clobbered.

## What changed in Test 5

**One additive change**, in `main/main.c` → `uart_relay_loop()`. Inserted a one-shot forensic block at the top of the relay loop. Runs once after `tick >= 300000` (roughly 3 seconds in, well after Core 1 has had time to wedge).

Other Test 4 changes preserved:
- DTS at `memory@4a000000`
- Regenerated `esp32p4_dtb.h`
- Test 3 diagnostics (dual-alias magic check, PMP snapshot)

Nothing removed. The scan is purely read-only — no PMP, no exec mapping, no DTB changes from Test 4. If Test 4's fix actually helped (kernel boots), we'd see lots of TX before the scan even runs, and the scan just provides a snapshot of `log_buf` for future reference.

## What I expect

Based on the "K then silence" pattern, I'm betting on **Outcome C**: "Linux version" found, "Kernel command line" not. That would mean the kernel got past the banner but died trying to format/print the command line. Most likely cause would be a `strlen()` or similar string operation on `saved_command_line` walking into bad memory.

If it's **Outcome B** (Kernel command line in log_buf but kernel still died), that proves the kernel ran far past 'K' — meaning earlycon's continued silence is a driver bug, not a kernel crash. That's important to know.

If it's **Outcome A** (5/5 markers), the kernel is basically working and we just have an earlycon driver issue.

## Files in this drop

```
test5_forensic_scan/
├── CMakeLists.txt           (unchanged)
├── README.md                (unchanged)
├── partitions.csv           (unchanged)
├── sdkconfig                (UNCHANGED)
├── sdkconfig.defaults       (unchanged)
├── components/benvisor/
│   ├── include/
│   │   ├── benvisor.h       (unchanged)
│   │   └── esp32p4_dtb.h    (Test 4 — memory@4a000000)
│   └── src/
│       ├── benvisor.c       (unchanged)
│       ├── benvisor_asm.S   (unchanged)
│       └── esp32p4.dts      (Test 4 — memory@4a000000)
└── main/
    └── main.c               (Test 4 + forensic scan in relay loop)
```

Flash, monitor, paste the `[FORENSIC]` lines. They'll be definitive.
