# benmmu-probe — Stage 0 hardware probes

Answers, on real ESP32-P4 silicon, the questions the BenMMU design (cache-MMU translation + PMP as a software-refilled TLB) depends on. Everything runs on Core 1 from a pinned FreeRTOS task, the same way BenLinux takes Core 1, so the probes see the PMP/PMA state Linux would see. During the probe window Core 1 has interrupts off and its own trap vector.

## Flash and run

Download the `benmmu-probe-firmware` artefact from the latest **Build BenLinux Firmware** run, or build locally (the Core 1 PMA patch must be applied to your IDF first):

```bash
cd firmware/benmmu-probe
../benlinux-standalone/tools/apply_pma_patch.sh $IDF_PATH
idf.py set-target esp32p4
idf.py build flash monitor
```

From the artefact: `esptool.py --chip esp32p4 -b 460800 write_flash @flash_args` from inside the unzipped folder.

The run takes a couple of seconds. Paste the full serial log from `=== BenMMU probe (Stage 0) ===` to `=== BenMMU probe finished ===` (or to wherever it stops).

Builds target chip rev v1.x (`sdkconfig.defaults`, IDF v5.5.1). For the v3.2 board this needs ESP-IDF v5.5.3+ and a v3 config; that comes with Stage 5.

## What each probe tells us

| Probe | Question | PASS means | If it fails |
| --- | --- | --- | --- |
| P0 | Which Core 1 PMP entries did ESP-IDF lock, and who governs PSRAM, IRAM, DRAM and peripherals for U-mode? | (INFO) PSRAM test pages ungoverned, at least 2 free entries | Probes P1–P3 cannot run; read the table |
| P4 | Do U-mode `ecall`, illegal-instruction and PMP load faults trap correctly on this silicon? | U-mode works for synchronous exceptions | No user-mode BenMMU on this rev |
| P1 | Does PMP match the CPU (virtual) address in the PSRAM window, before cache-MMU translation? Tested with two VAs mapped to one physical page, only one granted. | BenMMU's protection model works | **Kill criterion**: hardware route dead, binary-translation fallback |
| P2 | Are PMP load/store faults precise (`mepc` on the faulting `lw`/`sw`) and restartable after a grant? | Soft-TLB refill, COW and demand paging are possible | **Kill criterion** |
| P3 | With `mstatus.MPRV=1, MPP=U`, are M-mode loads/stores PMP-checked as U-mode? | Interim MMU on v1.x silicon is possible | No interim; wait for v3 U-mode |
| P5 | MMU geometry; does remapping one live entry A→B show page B after a cache invalidate, and is the cache tagged by CPU address? | Forkbank-style page switching works; flush needs confirmed | Page switching needs another approach |
| P6 | Cycle costs: PMP rewrite, U-mode fault round trip (the soft-TLB miss cost), MMU entry write, 64 KB writeback/invalidate, full page switch | (INFO) numbers for the design | — |
| P5d | What does an access through an invalidated MMU entry do: trap, garbage, hang, or cache-error interrupt? Runs last. | (INFO) | A hang or a "Cache error" panic after the window closes is itself the answer |

Set `PROBE_RUN_P5D` to `0` in `main/probe_main.c` to skip P5d.

## Known in advance

ESP-IDF locks most of Core 1's PMP entries before app code runs. Locked entries also apply to U-mode, so P4 is expected to report that U-mode can read internal SRAM. That is a protection gap for Stage 6 (fix: leave Core 1 PMP unlocked in the patched `cpu_region_protect.c`), not a blocker for these probes.
