# BenLinux

Native Linux on Core 1 of the ESP32-P4, hosted by the BenVisor M-mode shim, with Core 0 running the host firmware.

## Layout

| Path | What it is | CI |
| --- | --- | --- |
| `.github/workflows/build-kernel.yml`, `benlinux_kernel.cfg`, `configs/`, `patches/`, `timer-esp32p4.c`, `benvisor-console.c` | Linux 6.12 NOMMU kernel + initramfs, built with Buildroot. Patches 1–11 are applied inline by the workflow. | `Build BenLinux Kernel` (push to `main`, ~20 min; firmware-only changes skip it) |
| `firmware/benlinux-standalone/` | ESP-IDF v5.5.1 host firmware: Core 0 UART relay, BenVisor (`components/benvisor`), DTB, Core 1 Linux launch. `notes/` holds the May 2026 test and fix notes. | `Build BenLinux Firmware` (any change under `firmware/`) |

## Firmware build notes

- `sdkconfig` targets v1.x silicon (`CONFIG_ESP32P4_REV_MIN_1`). v3.x boards need ESP-IDF v5.5.3+ and a separate config.
- Runtime needs a patched `components/esp_hw_support/port/esp32p4/cpu_region_protect.c` in the IDF tree (PMA non-cacheable entries for Core 1: entry 0 CLIC at `0x20000000`, entry 12 HP peripherals at `0x50000000`). Run `firmware/benlinux-standalone/tools/apply_pma_patch.sh $IDF_PATH`; it only overwrites a stock v5.5.1 file. CI applies it automatically. The change itself is in `tools/esp-idf-patches/0001-esp32p4-core1-pma-noncacheable.patch`.
- `CONFIG_ESP_SYSTEM_HW_STACK_GUARD=n` is required (the stack guard fires on `li sp, 0` in Linux `head.S`).

## Status (May 2026)

Kernel ticks are delivered on vectored edge-triggered CLIC line 7. The open blocker is that the CLIC will not deliver hardware-raised interrupts while Core 1 is in U-mode on v1.x silicon; PATCH 11 runs userspace in M-mode as a workaround. v3.x silicon adds user-mode interrupt delegation and 32 PMP entries, so PATCH 11 is expected to be removed there.
