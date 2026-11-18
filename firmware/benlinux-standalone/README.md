# BenLinux Standalone

Native Linux on ESP32-P4 — dedicated chip, no other workloads.

## What This Is

A minimal ESP-IDF project that boots a Linux kernel on the ESP32-P4's Core 1. Core 0 acts as a UART console relay. No BLE, no WiFi, no display, no shell — just Linux.

## Hardware Requirements

- Any ESP32-P4 dev board with PSRAM (≥32MB recommended)
  - Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3"
  - Espressif ESP32-P4 Function EV Board
- SD card with kernel Image file
- UART0 serial connection (115200 baud)

## Quick Start

### 1. Prerequisites

- ESP-IDF v5.5.1 (or compatible)
- Linux kernel Image built for ESP32-P4 NOMMU (see `kernel/` directory)

### 2. Patch ESP-IDF (one-time)

The ESP32-P4 needs PMA (Physical Memory Attribute) entries set to non-cacheable for the CLIC interrupt controller and HP peripherals. Without this, CLIC MMIO reads return stale cached data.

**Option A — Automated patch:**
```bash
cd benlinux-standalone
./tools/apply_pma_patch.sh $IDF_PATH
```

**Option B — Manual copy:**
```bash
cp tools/esp-idf-patches/cpu_region_protect.c \
   $IDF_PATH/components/esp_hw_support/port/esp32p4/cpu_region_protect.c
```

The patch modifies `cpu_region_protect.c` to add per-core PMA entries:
- **Entry 0 (Core 1):** CPU subsystem `0x20000000` → non-cacheable (CLIC)
- **Entry 12 (Core 1):** HP peripherals `0x50000000` → non-cacheable (systimer/UART)
- Core 0 entries are unchanged.

### 3. Build & Flash

```bash
cd benlinux-standalone
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

### 4. Prepare SD Card

Place the kernel Image file at the root of an SD card:
```
/Image          ← kernel binary (built via Buildroot CI)
```

Insert the SD card before powering on.

### 5. Boot

Connect to UART0 at 115200 baud. You should see:

```
[benlinux] BenLinux Standalone
[benlinux] Native Linux on ESP32-P4 Core 1
[benlinux] PSRAM: 29 MB free
[benlinux] Loading kernel (4968500 bytes) to 0x48200000...
[CORE1] Jumping to kernel...
[    0.000000] Linux version 6.12.71 ...
[    0.100000] BusyBox v1.37.0 ...
/ #
```

Type commands at the `/ #` prompt.

## Architecture

```
┌─────────────────────────────────────────────────┐
│ ESP32-P4                                         │
│                                                   │
│  Core 0 (FreeRTOS)          Core 1 (Linux)        │
│  ┌──────────────────┐       ┌──────────────────┐  │
│  │ app_main()        │       │ Linux kernel     │  │
│  │   ↓               │       │ (M-mode, NOMMU)  │  │
│  │ Load kernel       │       │                  │  │
│  │   ↓               │       │ BusyBox shell    │  │
│  │ Launch Core 1     │       │ /dev/ttyBV0      │  │
│  │   ↓               │       │                  │  │
│  │ UART relay loop   │◄─────►│ IPC ring buffer  │  │
│  │  TX: IPC→UART0    │       │  TX: kernel→host │  │
│  │  RX: UART0→IPC    │       │  RX: host→kernel │  │
│  └──────────────────┘       └──────────────────┘  │
│                                                   │
│  PSRAM (0x48200000):  Kernel + BSS + DTB          │
│  HP SRAM:             IPC ring buffers (uncached)  │
└─────────────────────────────────────────────────┘
```

## Project Structure

```
benlinux-standalone/
├── CMakeLists.txt              ← ESP-IDF project
├── sdkconfig.defaults          ← Required sdkconfig settings
├── main/
│   ├── CMakeLists.txt
│   └── main.c                  ← Boot, load kernel, UART relay
├── components/
│   └── benvisor/
│       ├── CMakeLists.txt
│       ├── Kconfig
│       ├── include/
│       │   └── benvisor.h      ← Hardware constants, IPC layout
│       └── src/
│           ├── benvisor.c      ← CLIC/systimer setup
│           ├── benvisor_asm.S  ← mtvt table, timer ISR, boot trampoline
│           ├── esp32p4.dts     ← Device tree source
│           └── esp32p4_dtb.h   ← Compiled DTB (auto-generated)
├── kernel/                     ← Reference: kernel build files
│   ├── timer-esp32p4.c         ← Kernel clocksource/clockevent driver
│   ├── benvisor-console.c      ← Kernel tty/earlycon driver
│   ├── benlinux_kernel.cfg     ← Kernel config fragment
│   ├── benlinux_defconfig      ← Buildroot defconfig
│   └── patches/linux/          ← Kernel patches
├── tools/
│   └── patch_pma.sh            ← ESP-IDF PMA patch helper
└── README.md
```

## Key Design Decisions

**Why IPC ring buffer instead of direct UART0?**
The kernel console driver (`benvisor-console.c`) uses IPC ring buffers with ROM `putc` for earlycon output. This is the proven path from months of debugging. Direct UART0 from the kernel is a future optimization (requires verifying ESP32-P4 UART register compatibility with 8250/16550).

**Why WFI→NOP patching?**
ESP32-P4's WFI instruction may not wake on CLIC level-triggered interrupts. When the kernel enters the idle loop (WFI), timer interrupts stop being processed, killing all kernel timers. Replacing WFI with NOP makes idle busy-spin — wasteful but functional.

**Why GZIP not LZ4 initramfs?**
LZ4 decompression needs ~8MB contiguous allocation. NOMMU's `kmalloc` is limited by `MAX_ORDER` to ~4MB. GZIP uses a streaming 48KB buffer.

## Known Issues

1. **PMA entries are locked by ESP-IDF** before `app_main()`. The `cpu_region_protect.c` patch is mandatory. Run `./tools/apply_pma_patch.sh $IDF_PATH` or copy the pre-patched file.

2. **CLIC cache corruption** — CLIC register reads may return stale PSRAM data (e.g., `cfg=0x48695f44`). The CLIC hardware still works correctly; this only affects diagnostic reads. The kernel timer driver writes `0x00000007` to `cliccfg` as a workaround.

3. **No Linux shutdown** — NOMMU Linux has no clean shutdown path. Power cycle to restart.

4. **Single-core Linux only** — SMP Linux on both cores is architecturally possible but untested.

## Building the Kernel

The kernel is built via GitHub Actions CI (Buildroot + Linux 6.12 LTS). See the `kernel/` directory for:
- `benlinux_defconfig` — Buildroot configuration
- `benlinux_kernel.cfg` — Kernel config fragment
- `patches/linux/` — Required kernel patches
- `timer-esp32p4.c` — Custom timer driver (replaces timer-clint.c)
- `benvisor-console.c` — Custom console driver

Required CI patches (applied via sed in `build-kernel.yml`):
1. `head.S`: mtvec MODE=3 (CLIC mode) after every write
2. `entry.S`: mcause mask `& 0xFFF` before exception table lookup
3. `entry.S`: mcause save/restore around exception handling (CLIC MPIL)
4. `irq-riscv-intc.c`: mcause `& 0xFFF` mask
5. `timex.h`: CSR time instead of CLINT MMIO for `get_cycles()`
6. `Kconfig`: PAGE_OFFSET = 0x48200000

## Phase 2: Hybrid MMU (Future)

Once NOMMU is stable, BenVisor stays resident in M-mode providing trap-and-emulate Sv32 MMU:
- Linux boots in S-mode instead of M-mode
- `satp` writes → BenVisor reconfigures PMP entries
- Page faults → BenVisor walks page table, maps via PMP
- Enables `fork()`, `mmap()`, standard ELF binaries
