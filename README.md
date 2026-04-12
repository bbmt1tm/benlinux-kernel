# BenLinux Kernel

NOMMU Linux kernel for ESP32-P4 native execution (BenOS project).

## What This Builds

A Linux 6.12 LTS kernel Image with embedded BusyBox initramfs, configured for:
- **NOMMU** (no MMU, flat memory model)
- **M-mode** execution (no S-mode on ESP32-P4)
- **RV32IMA** (no compressed instructions)
- Custom **ESP32-P4 timer driver** (systimer via BenVisor)
- **LZ4** compressed initramfs

## How It Works

GitHub Actions CI builds the kernel using Buildroot 2025.02.10:
1. Downloads Linux 6.12.71 + Buildroot toolchain
2. Applies two kernel patches (timex.h CSR time, irq-riscv-intc.c CLIC mask)
3. Adds custom timer-esp32p4.c driver
4. Builds with NOMMU config fragment
5. Outputs `Image` file (upload to SD card as `/sd/vm/Image-nommu`)

## Kernel Patches

| Patch | File | Change |
|-------|------|--------|
| 0001 | `arch/riscv/include/asm/timex.h` | `get_cycles()` reads CSR time instead of CLINT MMIO (P4 has no CLINT) |
| 0002 | `drivers/irqchip/irq-riscv-intc.c` | mcause mask changed from 31-bit to 12-bit (CLIC mcause has extra status bits) |

## Custom Timer Driver

`timer-esp32p4.c` — clocksource + clock_event_device for ESP32-P4:
- **Clocksource:** reads CSR `time`/`timeh` (360 MHz CPU clock)
- **Clock events:** writes `mtimecmp` to shared SRAM (BenVisor polls this)
- **Interrupt:** IRQ 7 (machine timer), injected by BenVisor via CLIC pending bit

## ESP32-P4 Hardware Context

The ESP32-P4 has no standard CLINT timer and no S-mode. BenVisor (M-mode shim on Core 1) bridges the gap:
- Systimer Alarm 1 polls `mtimecmp` at ~10 kHz
- When `CSR time >= mtimecmp`, BenVisor sets CLIC int 7 pending bit
- CLIC hardware delivers a real timer interrupt to Linux
- Linux sees a standard machine timer interrupt (cause 7)

See `21_BENLINUX_SESSION_HANDOFF.md` in the BenOS main repo for full architecture details.

## Usage

Push to `main` or run workflow manually → download `benlinux-kernel-image` artifact → copy `Image` to SD card at `/sd/vm/Image-nommu`.
