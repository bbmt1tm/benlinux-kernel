/**
 * @file main.c
 * @brief BenLinux Standalone — Native Linux on ESP32-P4
 *
 * This is a dedicated-chip architecture: the ESP32-P4 boots,
 * loads a Linux kernel from SD card, launches it on Core 1,
 * and Core 0 becomes a UART console relay.
 *
 * Boot flow:
 *   1. app_main() on Core 0
 *   2. Mount SD card, load kernel Image to PSRAM
 *   3. Place + patch DTB in PSRAM
 *   4. Disable WDTs
 *   5. Launch Core 1 task (max priority, pinned)
 *   6. Core 1: disable FreeRTOS tick → benvisor_init → jump to kernel
 *   7. Core 0: loop draining IPC TX→UART0, feeding UART0 RX→IPC RX
 *
 * Console I/O:
 *   - Kernel earlycon: calls ROM putc directly (works with MIE=0)
 *   - Kernel ttyBV0:   writes to IPC TX ring buffer
 *   - Core 0 relay:    drains IPC TX → UART0 TX, reads UART0 RX → IPC RX
 *   - User sees:       standard serial terminal on UART0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_task_wdt.h"
#include "esp_cache.h"
#include "esp_rom_sys.h"
#include "esp_rom_uart.h"
#include "hal/wdt_hal.h"

/* SD card */
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

/* BenVisor */
#include "benvisor.h"
#include "esp32p4_dtb.h"
#include "esp_mmu_map.h"
#include "hal/mmu_types.h"

static const char *TAG = "benlinux";

/* ========================================
 * Forward Declarations
 * ======================================== */

static void IRAM_ATTR core1_linux_task(void *arg);
static void IRAM_ATTR uart_relay_loop(benvisor_ipc_t *ipc);

/* ========================================
 * BenVisor BSS globals (defined in benvisor_asm.S)
 *
 * Core 0 reads these via the uncached SRAM alias so we
 * see what Core 1 actually wrote, not stale L1.
 * ======================================== */

extern uint32_t benvisor_isr_count;
extern uint32_t benvisor_last_mepc;
extern uint32_t benvisor_diag_mtvec;
extern uint32_t benvisor_diag_mstatus;
extern uint32_t benvisor_saved_mcause;
extern uint32_t benvisor_heartbeat_ctr;
extern uint32_t benvisor_bail_count;   /* ticks that hit the sp==0 bail path */
extern uint32_t benvisor_bail_mepc;    /* PC running with sp==0 at last bail */

static inline uint32_t read_bv_nc(const uint32_t *cached_addr)
{
    return *(volatile uint32_t *)((uintptr_t)cached_addr + SOC_NON_CACHEABLE_OFFSET);
}

/* ========================================
 * Core 1 Boot Arguments
 * ======================================== */

typedef struct {
    uint32_t kernel_entry;
    uint32_t dtb_addr;
    benvisor_ipc_t *ipc;        /* uncached SRAM alias */
} linux_boot_args_t;

/* ========================================
 * PMA Verification
 *
 * Checks that the ESP-IDF PMA patch has been
 * applied. Without it, CLIC MMIO reads return
 * stale cached data → timer ISR livelock.
 * ======================================== */

static void check_pma_patch(void)
{
    /* Read PMA entry 0 and 12 from Core 0.
     * Core 1 has its own PMA entries (per-hart), so we can't
     * verify Core 1's from Core 0. But if the patch was applied
     * to cpu_region_protect.c, it modifies the entries for Core 1
     * during boot — we just check if the patch function exists
     * by looking for non-default values on Core 0.
     *
     * Note: Core 0's PMA entries may differ from Core 1's.
     * This is a best-effort check — the definitive check happens
     * in core1_linux_task after BenVisor init. */
    uint32_t pma_cfg0;
    asm volatile("csrr %0, 0xBC0" : "=r"(pma_cfg0));   /* pma_cfg0 */

    ESP_LOGI(TAG, "PMA check (Core 0): entry0 cfg=0x%08lx", (unsigned long)pma_cfg0);
    ESP_LOGI(TAG, "  Core 1 PMA will be verified after benvisor_init");

    /* WARNING: We can't fully validate from Core 0.
     * The Core 1 check in core1_linux_task is authoritative.
     * If you see "MISMATCH" there, the PMA patch is missing. */
}

/* ========================================
 * SD Card Mount
 *
 * Pin configuration for Waveshare ESP32-P4-WIFI6
 * dev board. If using a different board, update
 * the pin defines below.
 *
 * TODO: Confirm pins from research chat.
 * These are ESP32-P4 default SDMMC slot 1 pins.
 * ======================================== */

/* SD card SDMMC pins for Waveshare ESP32-P4 boards.
 * Confirmed working on Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3".
 * If using the original Waveshare ESP32-P4-WIFI6 dev kit or
 * Espressif EV Board, these may need updating. */
#define SD_SLOT             SDMMC_HOST_SLOT_0
#define SD_PIN_CLK          43
#define SD_PIN_CMD          44
#define SD_PIN_D0           39
#define SD_PIN_D1           40
#define SD_PIN_D2           41
#define SD_PIN_D3           42

static esp_err_t mount_sd_card(void)
{
    ESP_LOGI(TAG, "Mounting SD card...");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SD_SLOT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  /* 20MHz — reliable for bring-up */

    /* ESP32-P4 SD card IO pins need internal LDO power.
     * Without this, the pins have no power and the card won't respond. */
#if SOC_SDMMC_IO_POWER_EXTERNAL
    ESP_LOGI(TAG, "Configuring SD card IO power (LDO channel 4)...");
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,  /* LDO channel 4 for SD card IO */
    };
    static sd_pwr_ctrl_handle_t s_pwr_ctrl_handle = NULL;
    esp_err_t pwr_ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl_handle);
    if (pwr_ret != ESP_OK) {
        ESP_LOGE(TAG, "SD LDO power init failed: %s", esp_err_to_name(pwr_ret));
        return pwr_ret;
    }
    host.pwr_ctrl_handle = s_pwr_ctrl_handle;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0  = SD_PIN_D0;
    slot_config.d1  = SD_PIN_D1;
    slot_config.d2  = SD_PIN_D2;
    slot_config.d3  = SD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "SD pins: CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d (slot %d)",
             SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0,
             SD_PIN_D1, SD_PIN_D2, SD_PIN_D3, SD_SLOT);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot_config,
                                             &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGE(TAG, "Check SD card pins — may need updating for your board");
            ESP_LOGE(TAG, "Edit SD_PIN_* defines in main.c");
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

/* ========================================
 * Kernel Loading
 *
 * Loads kernel Image from SD card into PSRAM
 * at LINUX_PAGE_OFFSET. Zeros BSS region.
 * Places and patches DTB.
 * ======================================== */

static esp_err_t load_kernel_and_dtb(const char *kernel_path,
                                      benvisor_ipc_t *ipc_nc,
                                      uint32_t *out_entry,
                                      uint32_t *out_dtb)
{
    /* ---- PSRAM allocation ----
     * ESP-IDF heap starts at ~0x48000000. We allocate enough PSRAM
     * to cover from the heap start to LINUX_PAGE_OFFSET + LINUX_RAM_SIZE.
     * The kernel lives at LINUX_PAGE_OFFSET (0x48200000), safely past
     * all heap metadata (~46KB). */

    ESP_LOGI(TAG, "PSRAM: free=%zu KB, largest=%zu KB",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);

    void *psram_raw = heap_caps_malloc(LINUX_RAM_SIZE + LINUX_HEAP_EXTRA,
                                        MALLOC_CAP_SPIRAM);
    if (!psram_raw) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM (%d MB)",
                 (LINUX_RAM_SIZE + LINUX_HEAP_EXTRA) / (1024 * 1024));
        return ESP_ERR_NO_MEM;
    }

    uint32_t alloc_start = (uint32_t)psram_raw;
    uint32_t alloc_end = alloc_start + LINUX_RAM_SIZE + LINUX_HEAP_EXTRA;
    uint32_t psram_base = LINUX_PAGE_OFFSET;

    ESP_LOGI(TAG, "PSRAM alloc: 0x%08lx - 0x%08lx (%d MB)",
             (unsigned long)alloc_start, (unsigned long)alloc_end,
             (LINUX_RAM_SIZE + LINUX_HEAP_EXTRA) / (1024 * 1024));

    /* Verify PAGE_OFFSET falls within our allocation */
    if (psram_base < alloc_start || psram_base + LINUX_RAM_SIZE > alloc_end) {
        ESP_LOGE(TAG, "PAGE_OFFSET 0x%08lx not within allocation 0x%08lx-0x%08lx!",
                 (unsigned long)psram_base, (unsigned long)alloc_start,
                 (unsigned long)alloc_end);
        heap_caps_free(psram_raw);
        return ESP_FAIL;
    }

    /* ---- Load kernel from SD card ---- */
    FILE *f = fopen(kernel_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open kernel: %s", kernel_path);
        heap_caps_free(psram_raw);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long kernel_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (kernel_size <= 0 || kernel_size > (LINUX_RAM_SIZE - (1024 * 1024))) {
        ESP_LOGE(TAG, "Invalid kernel size: %ld", kernel_size);
        fclose(f);
        heap_caps_free(psram_raw);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Loading kernel (%ld bytes) to 0x%08lx...",
             kernel_size, (unsigned long)psram_base);
    size_t nread = fread((void *)psram_base, 1, kernel_size, f);
    fclose(f);

    if (nread != (size_t)kernel_size) {
        ESP_LOGE(TAG, "Kernel read incomplete: %zu / %ld", nread, kernel_size);
        heap_caps_free(psram_raw);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Kernel loaded (%ld bytes)", kernel_size);

    /* ---- Zero BSS region ----
     * CRITICAL: head.S reads hart_lottery from BSS BEFORE clearing it.
     * If hart_lottery is non-zero (garbage from unzeroed heap), the kernel
     * "loses" the lottery and parks in WFI forever. */
    size_t bss_zero_size = LINUX_RAM_SIZE - kernel_size;
    if (bss_zero_size > 0 && bss_zero_size < LINUX_RAM_SIZE) {
        memset((void *)(psram_base + kernel_size), 0, bss_zero_size);
        ESP_LOGI(TAG, "BSS zeroed: %zu KB", bss_zero_size / 1024);
    }

    /* ---- Flush dcache to PSRAM ---- */
    esp_err_t sync_err = esp_cache_msync((void *)psram_base, LINUX_RAM_SIZE,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    ESP_LOGI(TAG, "Cache flush: %s", sync_err == ESP_OK ? "OK" : esp_err_to_name(sync_err));

    /* ---- Make kernel executable at the SAME data address ----
     *
     * ESP32-P4 has UNIFIED I/D address space for PSRAM:
     *   SOC_MMU_DI_VADDR_SHARED=y, SOC_IRAM_PSRAM == SOC_DRAM_PSRAM.
     * MMU entries have NO R/W/X permission bits — any mapped page is
     * RWX at the hardware level. The W^X rejection inside esp_mmu_map's
     * s_mem_caps_check() is SOFTWARE POLICY only.
     *
     * What ACTUALLY blocks execute at 0x48xxxxxx is PMP — when
     * CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=y, cpu_region_protect.c locks
     * PMP entries 7-10 with RW (no X) for the PSRAM heap region, and
     * those entries are L=1 so even M-mode is restricted.
     *
     * With CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n in sdkconfig, those PMP
     * entries are NOT configured. The PSRAM heap region falls into an
     * unmatched PMP range → M-mode has full RWX. The heap's own MMU
     * entries at 0x48200000 are already unified I/D, so all we need is
     * fence.i to invalidate the icache so it picks up freshly-written
     * kernel code.
     *
     * Result: kernel runs at 0x48200000 (its load address = link address
     * via CONFIG_PAGE_OFFSET). No 0x4A alias, no exec mapping, no
     * cross-cache coherency to worry about. */
    asm volatile("fence.i");

    ESP_LOGI(TAG, "Kernel @ 0x%08lx: unified I/D mapping (no exec alias needed)",
             (unsigned long)psram_base);

    /* ---- Diagnostic: dump PMP entry 10 ----
     * Sanity check — if PMP_IDRAM_SPLIT really is disabled, entry 10
     * should be OFF (cfg=0) or at least not RW-locked over our region. */
    {
        uint32_t pmp10cfg_word, pmp10addr;
        /* pmpcfg2 holds entries 8-11 (4 entries × 1 byte each).
         * Read CSR 0x3A2 = pmpcfg2; entry 10's byte is bits [23:16]. */
        asm volatile("csrr %0, 0x3A2" : "=r"(pmp10cfg_word));
        asm volatile("csrr %0, 0x3BA" : "=r"(pmp10addr));   /* pmpaddr10 */
        uint8_t pmp10cfg = (pmp10cfg_word >> 16) & 0xFF;
        ESP_LOGI(TAG, "PMP[10]: cfg=0x%02x addr=0x%08lx %s",
                 pmp10cfg, (unsigned long)(pmp10addr << 2),
                 pmp10cfg == 0 ? "(OFF — PMP_IDRAM_SPLIT disabled, GOOD)"
                              : "(STILL CONFIGURED — check sdkconfig)");
    }

    /* ---- Place DTB in PSRAM ----
     * 15MB offset puts DTB safely past any possible kernel BSS.
     * DTB is only ~1KB. head.S clears BSS before parsing DTB — if DTB
     * overlaps BSS, it gets wiped → soc_early_init hangs.
     *
     * With unified I/D mapping, data and code use the SAME vaddr —
     * one address for memcpy and for kernel access. */
    uint32_t dtb_offset = 15 * 1024 * 1024;
    uint32_t dtb_data_addr = psram_base + dtb_offset;
    uint32_t dtb_addr = psram_base + dtb_offset;

    if (dtb_offset + ESP32P4_DTB_SIZE > LINUX_RAM_SIZE) {
        ESP_LOGE(TAG, "Kernel too large — no room for DTB");
        heap_caps_free(psram_raw);
        return ESP_ERR_NO_MEM;
    }
    memcpy((void *)dtb_data_addr, esp32p4_dtb, ESP32P4_DTB_SIZE);

    /* ---- Patch DTB: replace 0xCAFEBABE with IPC uncached SRAM address ---- */
    uint8_t *dtb = (uint8_t *)dtb_data_addr;
    uint32_t ipc_addr = (uint32_t)ipc_nc;
    bool ipc_patched = false;
    for (int i = 0; i < (int)ESP32P4_DTB_SIZE - 3; i++) {
        if (dtb[i] == 0xCA && dtb[i+1] == 0xFE &&
            dtb[i+2] == 0xBA && dtb[i+3] == 0xBE) {
            dtb[i+0] = (ipc_addr >> 24) & 0xFF;
            dtb[i+1] = (ipc_addr >> 16) & 0xFF;
            dtb[i+2] = (ipc_addr >>  8) & 0xFF;
            dtb[i+3] = (ipc_addr >>  0) & 0xFF;
            ipc_patched = true;
            ESP_LOGI(TAG, "DTB patched: IPC base = 0x%08lx",
                     (unsigned long)ipc_addr);
            break;
        }
    }
    if (!ipc_patched) {
        ESP_LOGW(TAG, "DTB: 0xCAFEBABE not found — console may not work");
    }

    ESP_LOGI(TAG, "DTB at 0x%08lx (%zu bytes)", (unsigned long)dtb_addr, (size_t)ESP32P4_DTB_SIZE);

    /* Flush DTB to PSRAM */
    esp_cache_msync((void *)dtb_data_addr, ESP32P4_DTB_SIZE,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    *out_entry = psram_base;          /* kernel entry = data vaddr (unified I/D) */
    *out_dtb = dtb_addr;
    return ESP_OK;
}

/* ========================================
 * Core 1 Linux Task
 *
 * Runs on Core 1 at max priority.
 * Disables FreeRTOS tick, WDTs, MIE,
 * initializes BenVisor, and boots Linux.
 * ======================================== */

static void IRAM_ATTR core1_linux_task(void *arg)
{
    linux_boot_args_t *args = (linux_boot_args_t *)arg;

    ESP_LOGI(TAG, "Core 1 task starting");
    ESP_LOGI(TAG, "  Kernel: 0x%08lx", (unsigned long)args->kernel_entry);
    ESP_LOGI(TAG, "  DTB:    0x%08lx", (unsigned long)args->dtb_addr);
    ESP_LOGI(TAG, "  IPC:    %p", args->ipc);

    /* Let Core 0 finish any pending cross-core operations */
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_rom_printf("[CORE1] Divorcing from FreeRTOS...\n");

    /* ---- Step 1: Disable Core 1 tick (systimer alarm 1) ---- */
    {
        uint32_t int_ena = *(volatile uint32_t *)0x500E2064;
        int_ena &= ~(1 << 1);  /* Clear bit 1 = alarm 1 */
        *(volatile uint32_t *)0x500E2064 = int_ena;
    }

    /* ---- Step 2: Wait for any in-flight ISR ---- */
    for (volatile int i = 0; i < 720000; i++) { }  /* ~2ms at 360MHz */

    /* ---- Step 3: Disable IWDT (MWDT1/TG1) ---- */
    {
        wdt_hal_context_t mwdt1_ctx;
        wdt_hal_init(&mwdt1_ctx, WDT_MWDT1, 0, false);
        wdt_hal_write_protect_disable(&mwdt1_ctx);
        wdt_hal_disable(&mwdt1_ctx);
        wdt_hal_write_protect_enable(&mwdt1_ctx);
    }

    /* ---- Step 4: Disable MIE — Core 1 goes dark ---- */
    asm volatile("csrc mstatus, 0x8");       /* Disable MIE */
    asm volatile("csrw mie, zero");          /* Disable all interrupt sources */

    esp_rom_printf("[CORE1] MIE disabled, entering benvisor_init\n");

    /* ---- Initialize BenVisor hardware ---- */
    benvisor_init(args->ipc);
    esp_rom_printf("[CORE1] benvisor_init done\n");

    /* ---- Diagnostic: mtvec/mtvt ---- */
    {
        uint32_t mtvec_val, mtvt_val;
        asm volatile("csrr %0, mtvec" : "=r"(mtvec_val));
        asm volatile("csrr %0, 0x307" : "=r"(mtvt_val));
        esp_rom_printf("[CORE1] mtvec=0x%08lx (MODE=%lu) mtvt=0x%08lx\n",
                       (unsigned long)mtvec_val, (unsigned long)(mtvec_val & 3),
                       (unsigned long)mtvt_val);
    }

    /* ---- PMA verification ---- */
    {
        uint32_t rb0_cfg, rb12_cfg;
        asm volatile("csrr %0, 0xBC0" : "=r"(rb0_cfg));    /* pma_cfg0 */
        asm volatile("csrr %0, 0xBCC" : "=r"(rb12_cfg));   /* pma_cfg12 */

        bool pma_ok = true;
        if (rb0_cfg != 0xE8000019) {
            esp_rom_printf("[PMA] ERROR: Entry 0 (CLIC) = 0x%08lx, expected 0xE8000019\n",
                           (unsigned long)rb0_cfg);
            esp_rom_printf("[PMA]   CLIC reads will return stale cached data!\n");
            pma_ok = false;
        }
        if (rb12_cfg != 0xE8000019) {
            esp_rom_printf("[PMA] ERROR: Entry 12 (periph) = 0x%08lx, expected 0xE8000019\n",
                           (unsigned long)rb12_cfg);
            esp_rom_printf("[PMA]   Systimer/UART reads may be stale!\n");
            pma_ok = false;
        }
        if (pma_ok) {
            esp_rom_printf("[PMA] OK: CLIC + peripherals non-cacheable\n");
        } else {
            esp_rom_printf("[PMA] PATCH MISSING — edit cpu_region_protect.c!\n");
            esp_rom_printf("[PMA]   Run: ./tools/apply_pma_patch.sh $IDF_PATH\n");
            esp_rom_printf("[PMA]   Continuing anyway — timer MAY still work.\n");
        }
    }

    /* ---- Pre-boot verification ---- */
    uint32_t first_insn = *(volatile uint32_t *)args->kernel_entry;
    uint32_t dtb_magic = *(volatile uint32_t *)args->dtb_addr;
    esp_rom_printf("[CORE1] Kernel @ 0x%08lx: first insn = 0x%08lx\n",
                   (unsigned long)args->kernel_entry, (unsigned long)first_insn);
    esp_rom_printf("[CORE1] DTB @ 0x%08lx: %s\n",
                   (unsigned long)args->dtb_addr,
                   dtb_magic == 0xEDFE0DD0 ? "VALID" : "BAD MAGIC");

    /* ---- Prepare for kernel jump ---- */
    asm volatile("csrc mstatus, 0x8");  /* MIE = 0 */

    /* Clear stale INT_ST, re-arm a fresh one-shot alarm.
     * With MIE=0, INT_ST stays asserted through all of head.S and
     * start_kernel(). When local_irq_enable() sets MIE=1, CLIC
     * sees INT_ST=1 (level-triggered) → delivers first timer interrupt. */
    *(volatile uint32_t *)0x500E206C = 2;  /* SYSTIMER_INT_CLR bit 1 */
    {
        uint32_t conf = *(volatile uint32_t *)0x500E2000;
        *(volatile uint32_t *)0x500E2000 = conf & ~(1 << 23);  /* TARGET1_EN = 0 */
        *(volatile uint32_t *)0x500E2004 = (1 << 30);          /* UNIT0_UPDATE */
        uint32_t now_lo = *(volatile uint32_t *)0x500E2044;
        *(volatile uint32_t *)0x500E2028 = now_lo + 1600;      /* TARGET1_LO */
        *(volatile uint32_t *)0x500E2054 = 1;                   /* COMP1_LOAD */
        *(volatile uint32_t *)0x500E2000 = conf | (1 << 23);   /* TARGET1_EN = 1 */
    }

    esp_rom_printf("[CORE1] Alarm re-armed, jumping with MIE=0\n");

    /* ---- Patch WFI → NOP ----
     * ESP32-P4's WFI may not wake on CLIC level-triggered interrupts.
     * Replace all WFI with NOP so idle loop busy-spins. */
    {
        volatile uint32_t *code = (volatile uint32_t *)args->kernel_entry;
        uint32_t kernel_words = 5 * 1024 * 1024 / 4;  /* scan first 5MB */
        int wfi_count = 0;
        for (uint32_t i = 0; i < kernel_words; i++) {
            if (code[i] == 0x10500073) {    /* WFI encoding */
                code[i] = 0x00000013;       /* NOP (addi x0, x0, 0) */
                wfi_count++;
            }
        }
        if (wfi_count > 0) {
            esp_cache_msync((void *)args->kernel_entry, 5 * 1024 * 1024,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            asm volatile("fence.i");
            esp_rom_printf("[CORE1] Patched %d WFI → NOP\n", wfi_count);
        }
    }

    /* ---- Jump to kernel (never returns for NOMMU) ---- */
    esp_rom_printf("[CORE1] Jumping to kernel...\n");
    benvisor_boot_linux(args->kernel_entry, args->dtb_addr, 1);

    /* If we ever get here (Linux shutdown), clean up */
    asm volatile("csrc mstatus, 0x8");
    asm volatile("csrw mie, zero");
    benvisor_cleanup();
    esp_rom_printf("[CORE1] Linux shut down, Core 1 idle\n");
    vTaskDelete(NULL);
}

/* ========================================
 * UART Relay Loop (Core 0)
 *
 * Drains IPC TX ring buffer → UART0 TX.
 * Reads UART0 RX → IPC RX ring buffer.
 *
 * This is the Core 0 main loop after Linux boots.
 * Uses ROM UART functions for minimal flash dependency.
 * ======================================== */

static void IRAM_ATTR uart_relay_loop(benvisor_ipc_t *ipc)
{
    /* We use both cached and uncached pointers:
     * - TX reads (Linux→host): cached pointer is fine (Linux writes, we read)
     * - RX writes (host→Linux): use uncached alias for cross-core visibility */
    benvisor_ipc_t *ipc_nc = (benvisor_ipc_t *)((uintptr_t)ipc + SOC_NON_CACHEABLE_OFFSET);

    /* ---- HEALTH state ----
     * Polling cadence is determined empirically: with 500000 outer iterations
     * and the busy work in this loop, HEALTH lands at ~5 Hz on a clean chip.
     * That's fast enough to catch a freeze transition within ~200 ms. */
    uint32_t tick = 0;

    /* Per-poll deltas */
    uint32_t last_alarm_target = 0;
    uint32_t last_tx_head      = 0;
    uint32_t last_isr_count    = 0;
    uint32_t last_hb_ctr       = 0;
    uint32_t last_bail_count   = 0;

    /* Freeze tracking */
    uint32_t stuck_polls      = 0;     /* polls where (STA && INT_ST&2 && isr_delta==0) */
    bool     freeze_announced = false;
    uint32_t inject_cooldown  = 0;     /* tick countdown between RX pokes */

    ESP_LOGI(TAG, "Core 0 entering UART relay loop (HEALTH-V2)");
    ESP_LOGI(TAG, "  IPC: %p (uncached: %p)", ipc, ipc_nc);
    ESP_LOGI(TAG, "  &isr_count cached=%p uncached=0x%08lx",
             &benvisor_isr_count,
             (unsigned long)((uintptr_t)&benvisor_isr_count + SOC_NON_CACHEABLE_OFFSET));
    ESP_LOGI(TAG, "  Type characters to send to Linux, output appears here");
    ESP_LOGI(TAG, "---");

    while (1) {
        /* ---- TX: drain Linux→UART0 (up to 64 chars per iteration) ---- */
        uint32_t tail = ipc->tx_tail;
        uint32_t head = ipc->tx_head;
        int drained = 0;
        while (tail != head && drained < 64) {
            char ch = ipc->tx_buf[tail];
            esp_rom_output_tx_one_char((uint8_t)ch);
            tail = (tail + 1) & IPC_TX_BUF_MASK;
            drained++;
        }
        if (drained > 0) {
            ipc->tx_tail = tail;
        }

        /* ---- RX: read UART0→Linux (one char per iteration) ---- */
        {
            uint8_t ch;
            if (esp_rom_uart_rx_one_char(&ch) == 0) {
                uint32_t rx_head = ipc_nc->rx_head;
                uint32_t rx_next = (rx_head + 1) & IPC_RX_BUF_MASK;
                if (rx_next != ipc_nc->rx_tail) {
                    ((volatile char *)ipc_nc->rx_buf)[rx_head] = (char)ch;
                    ipc_nc->rx_head = rx_next;
                }
            }
        }

        /* ---- HEALTH dump ---- */
        if (++tick >= 500000) {
            tick = 0;

            /* --- Systimer state (Core 0 can read these directly) --- */
            uint32_t alarm_target = *(volatile uint32_t *)0x500E2028;
            uint32_t int_st       = *(volatile uint32_t *)0x500E2070;
            uint32_t int_ena      = *(volatile uint32_t *)0x500E2064;
            uint32_t conf         = *(volatile uint32_t *)0x500E2000;

            /* Core 1 intmtx routing for systimer1 (memory-mapped, both cores see it) */
            uint32_t intmtx_c1    = *(volatile uint32_t *)INTMTX_SYSTIMER1_CORE1;

            /* --- BenVisor ISR-side state (uncached SRAM reads) ---
             * These are written by benvisor_timer_isr every time it runs.
             * If they stop changing, the ISR has stopped being invoked. */
            uint32_t isr_count    = read_bv_nc(&benvisor_isr_count);
            uint32_t last_mepc    = read_bv_nc(&benvisor_last_mepc);
            uint32_t diag_mtvec   = read_bv_nc(&benvisor_diag_mtvec);
            uint32_t diag_mstatus = read_bv_nc(&benvisor_diag_mstatus);
            uint32_t hb_ctr       = read_bv_nc(&benvisor_heartbeat_ctr);
            /* mcause restored by the ISR's csrw-before-mret (PATCH 9).
             * On this CLIC, mret reloads mintstatus.mil from this word's
             * MPIL field. If the last value at freeze carries a high MPIL
             * while the working ones read ~0, the running level is stuck
             * high and line 24 (level 7) can no longer preempt idle. */
            uint32_t saved_mcause = read_bv_nc(&benvisor_saved_mcause);
            uint32_t bail_count   = read_bv_nc(&benvisor_bail_count);
            uint32_t bail_mepc    = read_bv_nc(&benvisor_bail_mepc);

            /* --- Kernel trap-exit probe (PATCH 10) ---
             * ipc->probe[] is written by the kernel just before each mret.
             * The LAST value before a freeze is the return that crossed into
             * the failing context. probe[0] MPP tells user(0) vs kernel(3). */
            uint32_t pr_mstatus = ipc_nc->probe[0];
            uint32_t pr_mcause  = ipc_nc->probe[1];
            uint32_t pr_clic24  = ipc_nc->probe[2];
            uint32_t pr_thresh  = ipc_nc->probe[3];

            /* --- Kernel printk activity (IPC TX) --- */
            uint32_t tx_head_now = ipc->tx_head;

            /* --- Deltas vs previous poll --- */
            uint32_t target_delta = alarm_target - last_alarm_target;
            uint32_t isr_delta    = isr_count    - last_isr_count;
            uint32_t tx_delta     = tx_head_now  - last_tx_head;
            int32_t  hb_delta     = (int32_t)last_hb_ctr - (int32_t)hb_ctr;
            uint32_t bail_delta   = bail_count - last_bail_count;

            /* --- Freeze classification ---
             * A "stuck poll" = (isr_delta == 0) AND alarm fired (INT_ST bit 1).
             * If 3 stuck polls in a row, declare freeze.
             * If isr_delta > 0 again later, we've recovered. */
            bool stuck = (isr_delta == 0) && (int_st & 0x2);

            if (stuck) {
                stuck_polls++;
            } else {
                if (freeze_announced) {
                    esp_rom_printf("\n[H] RECOVERED — ISR firing again after %lu stuck polls\n",
                                   (unsigned long)stuck_polls);
                    freeze_announced = false;
                }
                stuck_polls = 0;
            }

            if (!freeze_announced && stuck_polls >= 3) {
                esp_rom_printf("\n");
                esp_rom_printf("=========================================\n");
                esp_rom_printf(" !! BenVisor ISR FREEZE DETECTED !!\n");
                esp_rom_printf("=========================================\n");
                esp_rom_printf(" Last successful ISR snapshot:\n");
                esp_rom_printf("   isr_count    = %lu\n",       (unsigned long)isr_count);
                esp_rom_printf("   last_mepc    = 0x%08lx   <-- KERNEL PC AT FREEZE (disasm here)\n",
                               (unsigned long)last_mepc);
                esp_rom_printf("   diag_mtvec   = 0x%08lx   <-- expected 0x484c78c3 (kernel mtvec)\n",
                               (unsigned long)diag_mtvec);
                esp_rom_printf("   diag_mstatus = 0x%08lx   <-- MIE bit3=%d, MPIE bit7=%d\n",
                               (unsigned long)diag_mstatus,
                               (diag_mstatus >> 3) & 1,
                               (diag_mstatus >> 7) & 1);
                esp_rom_printf("   saved_mcause = 0x%08lx   <-- mret restores mil from MPIL field\n",
                               (unsigned long)saved_mcause);
                esp_rom_printf("                  MPIL[31:24]=0x%02lx [23:16]=0x%02lx exc[11:0]=0x%03lx\n",
                               (unsigned long)((saved_mcause >> 24) & 0xff),
                               (unsigned long)((saved_mcause >> 16) & 0xff),
                               (unsigned long)(saved_mcause & 0xfff));
                esp_rom_printf("   bail_count   = %lu   <-- ticks taken while kernel sp==0\n",
                               (unsigned long)bail_count);
                esp_rom_printf("   bail_mepc    = 0x%08lx   <-- PC running with sp==0 (disasm/identify)\n",
                               (unsigned long)bail_mepc);
                esp_rom_printf(" Kernel trap-exit probe (last mret before freeze):\n");
                esp_rom_printf("   probe.mstatus= 0x%08lx   <-- MPP[12:11]=%lu (0=USER return, 3=kernel)\n",
                               (unsigned long)pr_mstatus,
                               (unsigned long)((pr_mstatus >> 11) & 3));
                esp_rom_printf("   probe.mcause = 0x%08lx   <-- MPIL[23:16]=0x%02lx (post-mret CLIC level)\n",
                               (unsigned long)pr_mcause,
                               (unsigned long)((pr_mcause >> 16) & 0xff));
                esp_rom_printf("   probe.CLIC24 = 0x%08lx   <-- ip=%lu ie=%lu attr=0x%02lx ctl=0x%02lx\n",
                               (unsigned long)pr_clic24,
                               (unsigned long)(pr_clic24 & 1),
                               (unsigned long)((pr_clic24 >> 8) & 1),
                               (unsigned long)((pr_clic24 >> 16) & 0xff),
                               (unsigned long)((pr_clic24 >> 24) & 0xff));
                esp_rom_printf("   probe.thresh = 0x%08lx   <-- mintthresh (expect 0)\n",
                               (unsigned long)pr_thresh);
                esp_rom_printf(" Systimer at freeze:\n");
                esp_rom_printf("   tgt          = 0x%08lx (frozen here)\n", (unsigned long)alarm_target);
                esp_rom_printf("   INT_ST       = 0x%lx  (bit 1 latched: alarm fired)\n",
                               (unsigned long)int_st);
                esp_rom_printf("   INT_ENA      = 0x%lx  (alarm 1 still enabled if bit 1 set)\n",
                               (unsigned long)int_ena);
                esp_rom_printf("   CONF         = 0x%08lx (TARGET1_EN bit 23 = %d)\n",
                               (unsigned long)conf, (int)((conf >> 23) & 1));
                esp_rom_printf("   INTMTX_C1    = 0x%08lx (should = 24 in low 6 bits)\n",
                               (unsigned long)intmtx_c1);
                esp_rom_printf(" Kernel liveness:\n");
                esp_rom_printf("   tx_head      = %lu (kernel printk position)\n",
                               (unsigned long)tx_head_now);
                esp_rom_printf("\n");
                esp_rom_printf(" Will now poke /bin/sh with '\\r' every ~5 polls.\n");
                esp_rom_printf(" If shell echoes anything (or tx_head advances),\n");
                esp_rom_printf(" the kernel is alive and only the ISR is dead.\n");
                esp_rom_printf(" If nothing comes back, kernel itself is frozen.\n");
                esp_rom_printf("=========================================\n");
                freeze_announced = true;
                inject_cooldown  = 5;
            }

            /* --- Compact per-poll status line ---
             * tgt:  systimer alarm 1 target (changes if ISR re-armed)
             * d=:   target delta since last poll
             * ST:   INT_ST bit 1 = alarm latched but not yet cleared
             * isr=: BenVisor isr_count delta since last poll (0 = dead)
             * tx=:  IPC TX head delta (kernel printk activity)
             * hbd=: heartbeat counter delta (positive = decrementing = ISR alive)
             */
            esp_rom_printf("[H] tgt=0x%08lx(%s d=0x%lx) ST=0x%lx isr=%lu tx=%lu hbd=%ld bail=%lu(d%lu) MPP=%lu c24=0x%08lx mepc=0x%08lx\n",
                           (unsigned long)alarm_target,
                           (target_delta == 0) ? "STA" : "CHG",
                           (unsigned long)target_delta,
                           (unsigned long)int_st,
                           (unsigned long)isr_delta,
                           (unsigned long)tx_delta,
                           (long)hb_delta,
                           (unsigned long)bail_count,
                           (unsigned long)bail_delta,
                           (unsigned long)((pr_mstatus >> 11) & 3),
                           (unsigned long)pr_clic24,
                           (unsigned long)last_mepc);

            last_alarm_target = alarm_target;
            last_isr_count    = isr_count;
            last_tx_head      = tx_head_now;
            last_hb_ctr       = hb_ctr;
            last_bail_count   = bail_count;

            /* --- After freeze: poke the shell to test kernel liveness --- */
            if (freeze_announced) {
                if (inject_cooldown > 0) {
                    inject_cooldown--;
                } else {
                    inject_cooldown = 5;  /* ~1s at 5Hz polling */
                    uint32_t rx_head = ipc_nc->rx_head;
                    uint32_t rx_next = (rx_head + 1) & IPC_RX_BUF_MASK;
                    if (rx_next != ipc_nc->rx_tail) {
                        ((volatile char *)ipc_nc->rx_buf)[rx_head] = '\r';
                        ipc_nc->rx_head = rx_next;
                        esp_rom_printf("[H] poke: \\r -> RX (rx_head=%lu); watch for echo / printk\n",
                                       (unsigned long)rx_next);
                    }
                }
            }
        }

        /* Tight polling — no vTaskDelay. Core 0 has nothing else to do.
         * A small NOP loop prevents the compiler from optimizing this into
         * a pure busy loop that starves ROM UART handling. */
        for (volatile int i = 0; i < 10; i++) { }
    }
}

/* ========================================
 * app_main — Entry Point
 * ======================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " BenLinux Standalone");
    ESP_LOGI(TAG, " Native Linux on ESP32-P4 Core 1");
    ESP_LOGI(TAG, "========================================");

    /* ---- Check PSRAM ---- */
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM: %zu MB free", psram_free / (1024 * 1024));
    if (psram_free < LINUX_RAM_SIZE) {
        ESP_LOGE(TAG, "Insufficient PSRAM (%zu MB < %d MB required)",
                 psram_free / (1024 * 1024), LINUX_RAM_SIZE / (1024 * 1024));
        return;
    }

    /* ---- Verify PMA patch ---- */
    check_pma_patch();

    /* ---- Mount SD card ---- */
    esp_err_t ret = mount_sd_card();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card required for kernel loading");
        ESP_LOGE(TAG, "Place kernel Image at /sd/Image and restart");
        return;
    }

    /* ---- Allocate IPC in internal SRAM ---- */
    benvisor_ipc_t *ipc = heap_caps_malloc(sizeof(benvisor_ipc_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ipc) {
        ESP_LOGE(TAG, "Failed to allocate IPC (%zu bytes)", sizeof(benvisor_ipc_t));
        return;
    }

    /* Compute uncached SRAM alias for cross-core coherency */
    benvisor_ipc_t *ipc_nc = (benvisor_ipc_t *)((uintptr_t)ipc + SOC_NON_CACHEABLE_OFFSET);
    ESP_LOGI(TAG, "IPC: %p (uncached: %p, %zu bytes)", ipc, ipc_nc, sizeof(benvisor_ipc_t));

    /* PATCH 10 hardcodes the kernel-side probe write address as an absolute
     * constant (0x8ff3c124 = uncached IPC base 0x8ff3b804 + 0x920). If the
     * allocator ever moves IPC, that constant is wrong — flag it loudly and
     * print the value to bake into the kernel patch. */
    {
        uint32_t probe_nc = (uint32_t)(uintptr_t)&ipc_nc->probe[0];
        ESP_LOGI(TAG, "PROBE scratch (uncached) = 0x%08lx", (unsigned long)probe_nc);
        if (probe_nc != 0x8ff3c124UL) {
            ESP_LOGW(TAG, "PROBE addr != 0x8ff3c124 — update PATCH 10 'li t2,0x%08lx'",
                     (unsigned long)probe_nc);
        }
    }

    /* Test uncached alias */
    {
        volatile uint32_t *test = (volatile uint32_t *)ipc_nc;
        *test = 0xA5A5A5A5;
        uint32_t rb = *test;
        ESP_LOGI(TAG, "Uncached SRAM test: %s", (rb == 0xA5A5A5A5) ? "OK" : "FAIL");
        *test = 0;
    }
    memset((void *)ipc_nc, 0, sizeof(*ipc_nc));

    /* ---- Load kernel and DTB ---- */
    const char *kernel_path = "/sd/Image";
    uint32_t kernel_entry, dtb_addr;

    ret = load_kernel_and_dtb(kernel_path, ipc_nc, &kernel_entry, &dtb_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Kernel load failed: %s", esp_err_to_name(ret));
        heap_caps_free(ipc);
        return;
    }

    /* ---- Disable WDTs from Core 0 BEFORE Core 1 takes over ----
     * If WDT fires after Core 1 enters Linux, the handler tries cross-core
     * IPC to Core 1 (never responds) → Core 0 deadlocks. */
    esp_task_wdt_deinit();
    ESP_LOGI(TAG, "Task WDT disabled");

    /* Disable Interrupt WDT (MWDT1/TG1) */
    {
        wdt_hal_context_t mwdt1_ctx;
        wdt_hal_init(&mwdt1_ctx, WDT_MWDT1, 0, false);
        wdt_hal_write_protect_disable(&mwdt1_ctx);
        wdt_hal_disable(&mwdt1_ctx);
        wdt_hal_write_protect_enable(&mwdt1_ctx);
    }
    ESP_LOGI(TAG, "Interrupt WDT (MWDT1) disabled");

    /* ---- Zero Core 0's systimer1 INTMTX routing (defensive) ---- */
    {
        uint32_t c0_route = *(volatile uint32_t *)INTMTX_SYSTIMER1_CORE0;
        if (c0_route != 0) {
            ESP_LOGW(TAG, "Core0 INTMTX[systimer1] was 0x%08lx — zeroing",
                     (unsigned long)c0_route);
            *(volatile uint32_t *)INTMTX_SYSTIMER1_CORE0 = 0;
        }
    }

    /* ---- Launch Core 1 task ---- */
    static linux_boot_args_t boot_args;
    boot_args.kernel_entry = kernel_entry;
    boot_args.dtb_addr = dtb_addr;
    boot_args.ipc = ipc_nc;  /* uncached alias */

    BaseType_t xret = xTaskCreatePinnedToCore(
        core1_linux_task, "linux_core1", 8192, &boot_args,
        configMAX_PRIORITIES - 1, NULL, 1);

    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Core 1 task");
        heap_caps_free(ipc);
        return;
    }

    ESP_LOGI(TAG, "Core 1 task launched — Linux is booting");
    ESP_LOGI(TAG, "Entering UART relay mode on Core 0...");

    /* ---- Small delay for Core 1 to start, then enter relay loop ----
     * The relay loop never returns. Core 0's sole job is UART↔IPC relay. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Enter relay loop — this never returns */
    uart_relay_loop(ipc);
}