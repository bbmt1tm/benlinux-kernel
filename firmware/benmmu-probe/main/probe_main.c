/*
 * BenMMU probe — Stage 0 hardware probes for BenLinux v2.
 *
 * Answers, on real silicon, the questions the BenMMU design depends on:
 *   P0  Core 1 PMP / PMA state as left by ESP-IDF (locked vs free entries)
 *   P4  U-mode exceptions: ecall, illegal instruction, PMP load fault
 *   P1  Does PMP check the CPU (virtual) address in the PSRAM window,
 *       before the cache-MMU translates it? (alias test)
 *   P2  Are PMP load/store faults precise and restartable?
 *   P3  Does mstatus.MPRV + MPP=U apply PMP to M-mode loads/stores?
 *   P5  Cache-MMU geometry, live remap of one entry, cache tagging
 *   P6  Timings: PMP rewrite, trap round trip, MMU entry write, cache ops
 *   P5d (last) Access through an invalidated MMU entry
 *
 * Everything runs on Core 1 from a pinned task. During the probe window
 * Core 1 has interrupts off and its own trap vector (probe_trap.S).
 * Each probe prints raw values and a verdict; one failure never hides the rest.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_mmu_map.h"
#include "esp_cache.h"
#include "hal/mmu_ll.h"
#include "hal/mmu_types.h"
#include "soc/soc.h"
#include "soc/ext_mem_defs.h"
#include "sdkconfig.h"
#include "probe.h"

#define H(x)            ((unsigned long)(x))
#define PAGE            0x10000u            /* 64 KB cache-MMU page */
#define FAULT_LOOP_N    1000
#define TIMING_N        1000
#define PROBE_RUN_P5D   1                   /* set 0 to skip the invalid-entry access */

#define PERM_R          0x01
#define PERM_W          0x02
#define PERM_X          0x04
#define PMP_A_NAPOT     0x18
#define PMP_L           0x80

#define SENT_A0         0xA11CE000u
#define SENT_A2         0xA11CE002u
#define SENT_B0         0xB0B0B000u

#define PSRAM_MMU       MMU_LL_PSRAM_MMU_ID

/* ------------------------------------------------------------------ */
/* CSR access                                                          */
/* ------------------------------------------------------------------ */
#define CSRR(csr)    ({ uint32_t __v; __asm__ volatile("csrr %0, " #csr : "=r"(__v)); __v; })
#define CSRW(csr, v) __asm__ volatile("csrw " #csr ", %0" :: "r"((uint32_t)(v)))

static uint32_t pmpaddr_get(int i)
{
    switch (i) {
    case 0:  return CSRR(0x3B0);  case 1:  return CSRR(0x3B1);
    case 2:  return CSRR(0x3B2);  case 3:  return CSRR(0x3B3);
    case 4:  return CSRR(0x3B4);  case 5:  return CSRR(0x3B5);
    case 6:  return CSRR(0x3B6);  case 7:  return CSRR(0x3B7);
    case 8:  return CSRR(0x3B8);  case 9:  return CSRR(0x3B9);
    case 10: return CSRR(0x3BA);  case 11: return CSRR(0x3BB);
    case 12: return CSRR(0x3BC);  case 13: return CSRR(0x3BD);
    case 14: return CSRR(0x3BE);  case 15: return CSRR(0x3BF);
    }
    return 0;
}

static void pmpaddr_set(int i, uint32_t v)
{
    switch (i) {
    case 0:  CSRW(0x3B0, v); break;  case 1:  CSRW(0x3B1, v); break;
    case 2:  CSRW(0x3B2, v); break;  case 3:  CSRW(0x3B3, v); break;
    case 4:  CSRW(0x3B4, v); break;  case 5:  CSRW(0x3B5, v); break;
    case 6:  CSRW(0x3B6, v); break;  case 7:  CSRW(0x3B7, v); break;
    case 8:  CSRW(0x3B8, v); break;  case 9:  CSRW(0x3B9, v); break;
    case 10: CSRW(0x3BA, v); break;  case 11: CSRW(0x3BB, v); break;
    case 12: CSRW(0x3BC, v); break;  case 13: CSRW(0x3BD, v); break;
    case 14: CSRW(0x3BE, v); break;  case 15: CSRW(0x3BF, v); break;
    }
}

static uint32_t pmpcfg_get(int r)
{
    switch (r) {
    case 0: return CSRR(0x3A0);  case 1: return CSRR(0x3A1);
    case 2: return CSRR(0x3A2);  case 3: return CSRR(0x3A3);
    }
    return 0;
}

static void pmpcfg_set(int r, uint32_t v)
{
    switch (r) {
    case 0: CSRW(0x3A0, v); break;  case 1: CSRW(0x3A1, v); break;
    case 2: CSRW(0x3A2, v); break;  case 3: CSRW(0x3A3, v); break;
    }
}

static uint32_t pmacfg_get(int i)
{
    switch (i) {
    case 0:  return CSRR(0xBC0);  case 1:  return CSRR(0xBC1);
    case 2:  return CSRR(0xBC2);  case 3:  return CSRR(0xBC3);
    case 4:  return CSRR(0xBC4);  case 5:  return CSRR(0xBC5);
    case 6:  return CSRR(0xBC6);  case 7:  return CSRR(0xBC7);
    case 8:  return CSRR(0xBC8);  case 9:  return CSRR(0xBC9);
    case 10: return CSRR(0xBCA);  case 11: return CSRR(0xBCB);
    case 12: return CSRR(0xBCC);  case 13: return CSRR(0xBCD);
    case 14: return CSRR(0xBCE);  case 15: return CSRR(0xBCF);
    }
    return 0;
}

static uint32_t pmaaddr_get(int i)
{
    switch (i) {
    case 0:  return CSRR(0xBD0);  case 1:  return CSRR(0xBD1);
    case 2:  return CSRR(0xBD2);  case 3:  return CSRR(0xBD3);
    case 4:  return CSRR(0xBD4);  case 5:  return CSRR(0xBD5);
    case 6:  return CSRR(0xBD6);  case 7:  return CSRR(0xBD7);
    case 8:  return CSRR(0xBD8);  case 9:  return CSRR(0xBD9);
    case 10: return CSRR(0xBDA);  case 11: return CSRR(0xBDB);
    case 12: return CSRR(0xBDC);  case 13: return CSRR(0xBDD);
    case 14: return CSRR(0xBDE);  case 15: return CSRR(0xBDF);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* PMP helpers                                                         */
/* ------------------------------------------------------------------ */
static uint8_t pmpcfg_byte(int i)
{
    return (pmpcfg_get(i / 4) >> ((i % 4) * 8)) & 0xFF;
}

static IRAM_ATTR void pmpcfg_byte_set(int i, uint8_t v)
{
    uint32_t r = pmpcfg_get(i / 4);
    r &= ~(0xFFu << ((i % 4) * 8));
    r |= (uint32_t)v << ((i % 4) * 8);
    pmpcfg_set(i / 4, r);
}

/* Grant perm on [base, base+size) using a NAPOT entry. size: power of two >= 8. */
static IRAM_ATTR void pmp_napot(int e, uint32_t base, uint32_t size, uint8_t perm)
{
    pmpcfg_byte_set(e, 0);
    pmpaddr_set(e, (base >> 2) | ((size >> 3) - 1));
    pmpcfg_byte_set(e, PMP_A_NAPOT | perm);
    __asm__ volatile("fence" ::: "memory");
}

static IRAM_ATTR void pmp_off(int e)
{
    pmpcfg_byte_set(e, 0);
    __asm__ volatile("fence" ::: "memory");
}

typedef struct {
    int      mode;      /* 0 OFF, 1 TOR, 2 NA4, 3 NAPOT */
    uint64_t lo, hi;    /* [lo, hi) */
} range_t;

static range_t pmp_decode(int i)
{
    range_t r = {0};
    uint8_t cfg = pmpcfg_byte(i);
    uint32_t a = pmpaddr_get(i);
    r.mode = (cfg >> 3) & 3;
    if (r.mode == 1) {
        r.lo = i ? ((uint64_t)pmpaddr_get(i - 1) << 2) : 0;
        r.hi = (uint64_t)a << 2;
    } else if (r.mode == 2) {
        r.lo = (uint64_t)a << 2;
        r.hi = r.lo + 4;
    } else if (r.mode == 3) {
        if (a == 0xFFFFFFFFu) {
            r.lo = 0;
            r.hi = 1ull << 34;
        } else {
            int t = __builtin_ctz(~a);
            r.lo = (uint64_t)(a & ~((1u << t) - 1)) << 2;
            r.hi = r.lo + (1ull << (t + 3));
        }
    }
    return r;
}

/* First active PMP entry matching addr, or -1. Lowest index has priority. */
static int pmp_match(uint32_t addr)
{
    for (int i = 0; i < 16; i++) {
        range_t r = pmp_decode(i);
        if (r.mode && addr >= r.lo && addr < r.hi) {
            return i;
        }
    }
    return -1;
}

static const char *perm_str(uint8_t cfg)
{
    static char s[5];
    s[0] = (cfg & PERM_R) ? 'R' : '-';
    s[1] = (cfg & PERM_W) ? 'W' : '-';
    s[2] = (cfg & PERM_X) ? 'X' : '-';
    s[3] = (cfg & PMP_L) ? 'L' : '-';
    s[4] = 0;
    return s;
}

static const char *mode_str(int m)
{
    static const char *names[] = { "OFF  ", "TOR  ", "NA4  ", "NAPOT" };
    return names[m & 3];
}

/* Free entries: unlocked, OFF, and not the base of a locked TOR entry above. */
static int s_free[16];
static int s_nfree;

static void pmp_find_free(void)
{
    s_nfree = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t c = pmpcfg_byte(i);
        bool locked = c & PMP_L;
        bool off = ((c >> 3) & 3) == 0;
        bool next_locked_tor = false;
        if (i < 15) {
            uint8_t n = pmpcfg_byte(i + 1);
            next_locked_tor = (n & PMP_L) && (((n >> 3) & 3) == 1);
        }
        if (!locked && off && !next_locked_tor) {
            s_free[s_nfree++] = i;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Trap handling                                                       */
/* ------------------------------------------------------------------ */
typedef enum { POL_SKIP, POL_FIXUP } policy_t;

typedef struct {
    uint32_t cause, epc, tval, mpp;
} trap_rec_t;

static volatile policy_t g_policy = POL_SKIP;
static bool (*volatile g_fixup)(trap_frame_t *f);
static volatile uint32_t g_run_traps;
static volatile uint32_t g_storm_limit = 32;
static volatile bool     g_storm;
static trap_rec_t        g_first, g_last;
uint32_t probe_saved_msp;

static IRAM_ATTR uint32_t insn_len(uint32_t pc)
{
    uint16_t lo = *(volatile uint16_t *)pc;
    return ((lo & 3) == 3) ? 4 : 2;
}

IRAM_ATTR int probe_trap_c(trap_frame_t *f)
{
    uint32_t code = f->mcause & 0xFFF;
    bool is_int = (f->mcause >> 31) & 1;
    uint32_t mpp = (f->mstatus >> 11) & 3;
    uint32_t n = ++g_run_traps;

    trap_rec_t rec = { .cause = is_int ? (0x80000000u | code) : code,
                       .epc = f->mepc, .tval = f->mtval, .mpp = mpp };
    if (n == 1) {
        g_first = rec;
    }
    g_last = rec;

    if (is_int) {
        return 0;                       /* not expected: MIE is 0 */
    }
    if (n > g_storm_limit) {
        g_storm = true;
        if (mpp == 0) {
            return 1;
        }
    }
    if (code == 8) {
        return 1;                       /* ecall from U: back to the probe */
    }
    if (code == 1) {                    /* instruction access fault */
        if (mpp == 0) {
            return 1;
        }
        esp_rom_printf("\n[TRAP] instruction access fault in M-mode at %08lx (mtval %08lx). Halting.\n",
                       H(f->mepc), H(f->mtval));
        for (;;) {
        }
    }
    if ((code == 5 || code == 7) && g_policy == POL_FIXUP && g_fixup && g_fixup(f)) {
        return 0;                       /* restart the same instruction */
    }
    f->mepc += insn_len(f->mepc);       /* skip it */
    return 0;
}

static void reset_run(void)
{
    g_run_traps = 0;
    g_storm = false;
    memset(&g_first, 0, sizeof(g_first));
    memset(&g_last, 0, sizeof(g_last));
}

/* ------------------------------------------------------------------ */
/* Probe environment and user-mode runner                              */
/* ------------------------------------------------------------------ */
static uint8_t s_trap_stack[3072] __attribute__((aligned(16)));
static uint8_t s_user_stack[2048] __attribute__((aligned(16)));
static uint32_t s_env_mstatus, s_env_mtvec, s_env_mscratch;

static void probe_env_enter(void)
{
    uint32_t ms;
    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(ms));
    s_env_mstatus = ms;
    s_env_mtvec = CSRR(mtvec);
    CSRW(mtvec, ((uint32_t)probe_trap_vector & ~0x3Fu) | (s_env_mtvec & 0x3Fu));
    s_env_mscratch = CSRR(mscratch);
    CSRW(mscratch, (uint32_t)(s_trap_stack + sizeof(s_trap_stack)));
}

static void probe_env_exit(void)
{
    CSRW(mscratch, s_env_mscratch);
    CSRW(mtvec, s_env_mtvec);
    if (s_env_mstatus & 8) {
        __asm__ volatile("csrsi mstatus, 8");
    }
}

/* Runs fn(arg) in U-mode. Returns the number of faults (traps other than the final ecall). */
static uint32_t run_user(void (*fn)(uint32_t), uint32_t arg)
{
    reset_run();
    probe_run_user((uint32_t)fn, (uint32_t)(s_user_stack + sizeof(s_user_stack)), arg);
    uint32_t traps = g_run_traps;
    return (traps && g_last.cause == 8) ? traps - 1 : traps;
}

/* ------------------------------------------------------------------ */
/* U-mode test functions (IRAM; reachable from U via IDF's locked       */
/* internal-SRAM PMP entry, which P0 checks)                            */
/* ------------------------------------------------------------------ */
static volatile uint32_t u_result;
static volatile uint32_t s_sram_word = 0x5A5A1234u;
extern char p2_load_pc[], p2_store_pc[];

static IRAM_ATTR __attribute__((noinline)) void u_nop(uint32_t a)
{
    (void)a;
}

static IRAM_ATTR __attribute__((noinline)) void u_illegal(uint32_t a)
{
    (void)a;
    __asm__ volatile("csrr t0, mstatus" ::: "t0");     /* illegal in U-mode */
    u_result = 0x55;
}

static IRAM_ATTR __attribute__((noinline)) void u_load(uint32_t a)
{
    u_result = *(volatile uint32_t *)a;
}

static IRAM_ATTR __attribute__((noinline)) void u_load_marked(uint32_t a)
{
    uint32_t v;
    __asm__ volatile(".global p2_load_pc\np2_load_pc:\n\tlw %0, 0(%1)"
                     : "=r"(v) : "r"(a) : "memory");
    u_result = v;
}

static IRAM_ATTR __attribute__((noinline)) void u_store_marked(uint32_t a)
{
    __asm__ volatile(".global p2_store_pc\np2_store_pc:\n\tsw %1, 0(%0)"
                     :: "r"(a), "r"(0xC0FFEE22u) : "memory");
}

static IRAM_ATTR __attribute__((noinline)) void u_fault_loop(uint32_t a)
{
    for (int i = 0; i < FAULT_LOOP_N; i++) {
        (void)*(volatile uint32_t *)a;
    }
}

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */
typedef enum { R_NOTRUN, R_PASS, R_FAIL, R_INCONC, R_INFO } res_t;
enum { P0, P1, P2, P3, P4, P5, P6, P5D, NPROBES };
static const char *s_names[NPROBES] = {
    "P0  Core 1 PMP/PMA state",
    "P1  PMP checks CPU (virtual) address",
    "P2  PMP faults precise + restartable",
    "P3  MPRV applies PMP to M-mode data",
    "P4  U-mode exceptions",
    "P5  Cache-MMU remap + cache tagging",
    "P6  Timings",
    "P5d Invalid MMU entry access",
};
static res_t s_res[NPROBES];

static const char *res_str(res_t r)
{
    switch (r) {
    case R_PASS:   return "PASS";
    case R_FAIL:   return "FAIL";
    case R_INCONC: return "INCONCLUSIVE";
    case R_INFO:   return "INFO";
    default:       return "NOT RUN";
    }
}

static void chk(const char *what, bool ok)
{
    esp_rom_printf("  [%s] %s\n", ok ? "ok" : "!!", what);
}

static void print_trap(const char *label, const trap_rec_t *t)
{
    esp_rom_printf("    %s: mcause=%lu mepc=%08lx mtval=%08lx prev_mode=%s\n", label,
                   H(t->cause), H(t->epc), H(t->tval),
                   t->mpp == 3 ? "M" : (t->mpp == 0 ? "U" : "?"));
}

static inline uint32_t cyc(void)
{
    return (uint32_t)esp_cpu_get_cycle_count();
}

static void print_cycles(const char *what, uint32_t cycles, uint32_t n)
{
    uint32_t per = n ? cycles / n : cycles;
    uint32_t ns = per * 1000u / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    esp_rom_printf("    %s %lu cycles  (%lu ns)\n", what, H(per), H(ns));
}

/* ------------------------------------------------------------------ */
/* Shared test memory                                                  */
/* ------------------------------------------------------------------ */
static uint8_t *s_buf;          /* 2 x 64 KB PSRAM pages, A then B */
static uint32_t s_A, s_B;       /* heap virtual addresses */
static uint32_t s_pA, s_pB;     /* physical addresses */
static uint32_t s_alias;        /* second VA for page A (esp_mmu_map) */
static uint32_t s_alias2;       /* second VA for page B (used by P5d) */
static int e0 = -1, e1 = -1;    /* free PMP entries used by the probes */
static bool s_u_ok, s_psram_denied;

static bool setup_memory(void)
{
    s_buf = heap_caps_aligned_alloc(PAGE, 2 * PAGE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        esp_rom_printf("setup: PSRAM alloc failed\n");
        return false;
    }
    s_A = (uint32_t)s_buf;
    s_B = s_A + PAGE;
    memset(s_buf, 0, 2 * PAGE);
    volatile uint32_t *a = (volatile uint32_t *)s_A, *b = (volatile uint32_t *)s_B;
    a[0] = SENT_A0;
    a[2] = SENT_A2;
    b[0] = SENT_B0;
    esp_cache_msync(s_buf, 2 * PAGE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    mmu_target_t tgt;
    if (esp_mmu_vaddr_to_paddr((void *)s_A, &s_pA, &tgt) != ESP_OK ||
        esp_mmu_vaddr_to_paddr((void *)s_B, &s_pB, &tgt) != ESP_OK) {
        esp_rom_printf("setup: vaddr_to_paddr failed\n");
        return false;
    }
    const mmu_mem_caps_t caps = MMU_MEM_CAP_READ | MMU_MEM_CAP_WRITE | MMU_MEM_CAP_8BIT | MMU_MEM_CAP_32BIT;
    void *p = NULL;
    if (esp_mmu_map(s_pA, PAGE, MMU_TARGET_PSRAM0, caps, ESP_MMU_MMAP_FLAG_PADDR_SHARED, &p) != ESP_OK) {
        esp_rom_printf("setup: esp_mmu_map alias of A failed\n");
        return false;
    }
    s_alias = (uint32_t)p;
    if (esp_mmu_map(s_pB, PAGE, MMU_TARGET_PSRAM0, caps, ESP_MMU_MMAP_FLAG_PADDR_SHARED, &p) != ESP_OK) {
        esp_rom_printf("setup: esp_mmu_map alias of B failed\n");
        return false;
    }
    s_alias2 = (uint32_t)p;

    esp_rom_printf("setup: page A va=%08lx pa=%08lx | page B va=%08lx pa=%08lx\n",
                   H(s_A), H(s_pA), H(s_B), H(s_pB));
    esp_rom_printf("setup: alias(A) va=%08lx  alias2(B) va=%08lx\n", H(s_alias), H(s_alias2));
    uint32_t v = *(volatile uint32_t *)s_alias;
    esp_rom_printf("setup: alias(A)[0]=%08lx (expect %08lx) -> alias %s\n",
                   H(v), H(SENT_A0), v == SENT_A0 ? "works" : "BROKEN");
    return v == SENT_A0;
}

/* ------------------------------------------------------------------ */
/* P0 — Core 1 PMP / PMA state                                         */
/* ------------------------------------------------------------------ */
static void probe_p0(void)
{
    esp_rom_printf("\n== P0: Core 1 PMP / PMA state ==\n");
    esp_rom_printf("  mhartid=%lu misa=%08lx mstatus=%08lx mtvec=%08lx\n",
                   H(CSRR(mhartid)), H(CSRR(misa)), H(CSRR(mstatus)), H(CSRR(mtvec)));

    esp_rom_printf("  PMP (lowest index wins):\n");
    for (int i = 0; i < 16; i++) {
        uint8_t c = pmpcfg_byte(i);
        range_t r = pmp_decode(i);
        if (r.mode) {
            esp_rom_printf("   %2d %s %s  %08lx-%08lx  (addr=%08lx cfg=%02x)\n", i, mode_str(r.mode),
                           perm_str(c), H((uint32_t)r.lo), H((uint32_t)(r.hi - 1)), H(pmpaddr_get(i)), c);
        } else {
            esp_rom_printf("   %2d OFF   %s  (addr=%08lx cfg=%02x)\n", i, perm_str(c), H(pmpaddr_get(i)), c);
        }
    }

    pmp_find_free();
    esp_rom_printf("  free (unlocked, OFF) entries: %d ->", s_nfree);
    for (int i = 0; i < s_nfree; i++) {
        esp_rom_printf(" %d", s_free[i]);
    }
    esp_rom_printf("\n");

    if (s_nfree) {
        int g = s_free[0];
        uint32_t save = pmpaddr_get(g);
        pmpaddr_set(g, 0xFFFFFFFFu);
        uint32_t rb = pmpaddr_get(g);
        pmpaddr_set(g, save);
        int gbits = rb ? __builtin_ctz(rb) : -1;
        esp_rom_printf("  PMP granularity: pmpaddr readback %08lx -> %lu bytes\n",
                       H(rb), gbits >= 0 ? H(1ul << (gbits + 2)) : 0ul);
    }

    const struct { const char *name; uint32_t addr; } probes[] = {
        { "PSRAM window start 0x48000000", SOC_EXTRAM_LOW },
        { "test page A (PSRAM heap)", s_A },
        { "alias of A (esp_mmu_map)", s_alias },
        { "U-mode code (u_load, IRAM)", (uint32_t)u_load },
        { "U-mode stack (DRAM)", (uint32_t)s_user_stack },
        { "U-mode result word (DRAM)", (uint32_t)&u_result },
        { "HP peripherals 0x500E2000 (systimer)", 0x500E2000u },
    };
    esp_rom_printf("  Who governs U-mode access to:\n");
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        int m = pmp_match(probes[i].addr);
        if (m < 0) {
            esp_rom_printf("   %s %08lx -> no entry (U-mode denied by default)\n",
                           probes[i].name, H(probes[i].addr));
        } else {
            esp_rom_printf("   %s %08lx -> entry %d %s\n", probes[i].name, H(probes[i].addr), m,
                           perm_str(pmpcfg_byte(m)));
        }
    }

    esp_rom_printf("  PMA:\n");
    for (int i = 0; i < 16; i++) {
        uint32_t c = pmacfg_get(i);
        if (!c) {
            continue;
        }
        esp_rom_printf("   %2d cfg=%08lx addr=%08lx  %s%s%s%s%s%s\n", i, H(c), H(pmaaddr_get(i)),
                       (c >> 30) == 1 ? "TOR " : (c >> 30) == 2 ? "NA4 " : (c >> 30) == 3 ? "NAPOT " : "",
                       (c & (1u << 4)) ? "R" : "-", (c & (1u << 3)) ? "W" : "-", (c & (1u << 2)) ? "X" : "-",
                       (c & (1u << 27)) ? " NONCACHEABLE" : "", (c & (1u << 29)) ? " L" : "");
    }

    bool psram_free = pmp_match(s_A) < 0 && pmp_match(s_alias) < 0;
    chk("PSRAM test pages are not covered by any PMP entry", psram_free);
    chk("at least 2 free PMP entries for the probes", s_nfree >= 2);
    if (s_nfree >= 2) {
        e0 = s_free[s_nfree - 2];
        e1 = s_free[s_nfree - 1];
        esp_rom_printf("  probes will use PMP entries %d and %d\n", e0, e1);
    }
    s_res[P0] = (psram_free && s_nfree >= 2) ? R_INFO : R_FAIL;
}

/* ------------------------------------------------------------------ */
/* P4 — U-mode exceptions                                              */
/* ------------------------------------------------------------------ */
static void probe_p4(void)
{
    esp_rom_printf("\n== P4: U-mode exceptions ==\n");
    uint32_t f;

    f = run_user(u_nop, 0);
    bool a = g_run_traps == 1 && g_last.cause == 8 && g_last.mpp == 0;
    print_trap("ecall", &g_last);
    chk("U-mode entry and ecall return (mcause 8, prev mode U)", a);

    u_result = 0;
    f = run_user(u_illegal, 0);
    bool b = f == 1 && g_first.cause == 2 && g_first.mpp == 0 && u_result == 0x55;
    print_trap("first", &g_first);
    chk("illegal CSR read in U traps (mcause 2) and resumes", b);

    u_result = 0;
    f = run_user(u_load, (uint32_t)&s_sram_word);
    bool c = f == 0 && u_result == 0x5A5A1234u;
    chk("U-mode can read internal SRAM (via IDF's locked entry)", c);
    if (c) {
        esp_rom_printf("    finding: U-mode has access to internal SRAM; BenMMU isolation needs Core 1 PMP unlocked\n");
    }

    bool d = false;
    if (pmp_match(s_A) < 0) {
        u_result = 0;
        f = run_user(u_load, s_A);
        d = f >= 1 && g_first.cause == 5 && g_first.tval == s_A && g_first.mpp == 0;
        print_trap("first", &g_first);
    }
    chk("U-mode load from ungranted PSRAM faults (mcause 5, mtval = address)", d);
    if (g_storm) {
        esp_rom_printf("    WARNING: fault storm hit the limit\n");
    }

    s_u_ok = a;
    s_psram_denied = d;
    s_res[P4] = (a && b && d) ? R_PASS : R_FAIL;
}

/* ------------------------------------------------------------------ */
/* P1 — PMP checks CPU address (alias test)                            */
/* ------------------------------------------------------------------ */
static void probe_p1(void)
{
    esp_rom_printf("\n== P1: does PMP check the CPU (virtual) address? ==\n");
    if (!s_u_ok || !s_psram_denied || e0 < 0) {
        esp_rom_printf("  skipped: needs P4 user mode, PSRAM default-deny and free PMP entries\n");
        s_res[P1] = R_INCONC;
        return;
    }
    uint32_t f;

    pmp_napot(e0, s_A, PAGE, PERM_R);
    u_result = 0;
    f = run_user(u_load, s_A);
    bool granted_ok = f == 0 && u_result == SENT_A0;
    chk("U load from granted VA of page A succeeds", granted_ok);

    u_result = 0;
    f = run_user(u_load, s_alias);
    bool alias_faults = f >= 1 && g_first.cause == 5 && g_first.tval == s_alias;
    bool alias_leaks = f == 0 && u_result == SENT_A0;
    if (f) {
        print_trap("alias", &g_first);
    }
    chk("U load from alias VA (same physical page, no grant) faults", alias_faults);

    pmp_napot(e1, s_alias, PAGE, PERM_R);
    u_result = 0;
    f = run_user(u_load, s_alias);
    bool control = f == 0 && u_result == SENT_A0;
    chk("control: granting the alias VA makes the alias load succeed", control);

    pmp_off(e0);
    pmp_off(e1);

    if (granted_ok && alias_faults && control) {
        s_res[P1] = R_PASS;
        esp_rom_printf("  => PMP matches the CPU address before cache-MMU translation\n");
    } else if (granted_ok && alias_leaks) {
        s_res[P1] = R_FAIL;
        esp_rom_printf("  => alias read succeeded without a grant: PMP does not match the CPU address\n");
    } else {
        s_res[P1] = R_INCONC;
    }
}

/* ------------------------------------------------------------------ */
/* P2 — precise, restartable faults                                    */
/* ------------------------------------------------------------------ */
static volatile uint32_t s_fix_base, s_fix_epc, s_fix_code, s_fix_count;
static volatile int s_fix_entry;

static IRAM_ATTR bool fix_grant(trap_frame_t *f)
{
    if (f->mtval < s_fix_base || f->mtval >= s_fix_base + PAGE) {
        return false;
    }
    uint32_t code = f->mcause & 0xFFF;
    s_fix_epc = f->mepc;
    s_fix_code = code;
    s_fix_count++;
    pmp_napot(s_fix_entry, s_fix_base, PAGE, code == 7 ? (PERM_R | PERM_W) : PERM_R);
    return true;
}

static void probe_p2(void)
{
    esp_rom_printf("\n== P2: precise + restartable PMP faults ==\n");
    if (!s_u_ok || !s_psram_denied || e1 < 0) {
        esp_rom_printf("  skipped: needs P4 user mode, PSRAM default-deny and free PMP entries\n");
        s_res[P2] = R_INCONC;
        return;
    }

    g_fixup = fix_grant;
    g_policy = POL_FIXUP;
    s_fix_base = s_alias;
    s_fix_entry = e1;

    s_fix_count = 0;
    u_result = 0;
    uint32_t f = run_user(u_load_marked, s_alias + 8);
    bool l_pc = s_fix_epc == (uint32_t)p2_load_pc;
    bool l_ok = s_fix_count == 1 && f == 1 && l_pc && s_fix_code == 5 && u_result == SENT_A2;
    esp_rom_printf("    load: faults=%lu fix_epc=%08lx expected=%08lx value=%08lx (expect %08lx)\n",
                   H(f), H(s_fix_epc), H((uint32_t)p2_load_pc), H(u_result), H(SENT_A2));
    chk("load fault: mepc = faulting lw, restart after grant returns the right value", l_ok);
    pmp_off(e1);

    s_fix_count = 0;
    f = run_user(u_store_marked, s_alias + 16);
    uint32_t written = *(volatile uint32_t *)(s_alias + 16);
    bool s_ok = s_fix_count == 1 && f == 1 && s_fix_epc == (uint32_t)p2_store_pc && s_fix_code == 7 &&
                written == 0xC0FFEE22u;
    esp_rom_printf("    store: faults=%lu fix_epc=%08lx expected=%08lx readback=%08lx\n",
                   H(f), H(s_fix_epc), H((uint32_t)p2_store_pc), H(written));
    chk("store fault: mcause 7, mepc = faulting sw, restart completes the store", s_ok);
    pmp_off(e1);

    g_policy = POL_SKIP;
    g_fixup = NULL;
    s_res[P2] = (l_ok && s_ok) ? R_PASS : R_FAIL;
}

/* ------------------------------------------------------------------ */
/* P3 — MPRV                                                           */
/* ------------------------------------------------------------------ */
static void probe_p3(void)
{
    esp_rom_printf("\n== P3: MPRV applies PMP to M-mode loads/stores ==\n");
    if (e0 < 0 || pmp_match(s_A) >= 0) {
        esp_rom_printf("  skipped: needs a free PMP entry and an ungoverned PSRAM page\n");
        s_res[P3] = R_INCONC;
        return;
    }
    uint32_t v;

    reset_run();
    v = probe_mprv_load(s_A, 3);
    bool m_ok = g_run_traps == 0 && v == SENT_A0;
    chk("control: MPRV=1, MPP=M load succeeds", m_ok);

    reset_run();
    v = probe_mprv_load(s_A, 0);
    bool u_denied = g_run_traps == 1 && g_first.cause == 5 && g_first.tval == s_A &&
                    g_first.epc == (uint32_t)probe_mprv_load_pc;
    if (g_run_traps) {
        print_trap("trap", &g_first);
    }
    chk("MPRV=1, MPP=U load from ungranted PSRAM faults precisely", u_denied);

    pmp_napot(e0, s_A, PAGE, PERM_R);
    reset_run();
    v = probe_mprv_load(s_A, 0);
    bool u_ok = g_run_traps == 0 && v == SENT_A0;
    chk("MPRV=1, MPP=U load from R-granted PSRAM succeeds", u_ok);

    reset_run();
    probe_mprv_store(s_A + 32, 0, 0x12345678u);
    bool st_denied = g_run_traps == 1 && g_first.cause == 7 && g_first.epc == (uint32_t)probe_mprv_store_pc;
    if (g_run_traps) {
        print_trap("trap", &g_first);
    }
    chk("MPRV=1, MPP=U store to R-only PSRAM faults (mcause 7)", st_denied);
    pmp_off(e0);

    uint32_t ms = CSRR(mstatus);
    chk("MPRV is clear afterwards", (ms & 0x20000u) == 0);

    s_res[P3] = (m_ok && u_denied && u_ok && st_denied) ? R_PASS : R_FAIL;
}

/* ------------------------------------------------------------------ */
/* P5 — cache-MMU geometry, remap, cache tagging                       */
/* ------------------------------------------------------------------ */
static uint32_t mmu_val(uint32_t paddr)
{
    return mmu_ll_format_paddr(PSRAM_MMU, paddr, MMU_TARGET_PSRAM0);
}

static void probe_p5(void)
{
    esp_rom_printf("\n== P5: cache-MMU geometry, remap, cache tagging ==\n");
    size_t free_va = 0;
    esp_mmu_map_get_max_consecutive_free_block_size(MMU_MEM_CAP_READ | MMU_MEM_CAP_WRITE |
                                                    MMU_MEM_CAP_8BIT | MMU_MEM_CAP_32BIT,
                                                    MMU_TARGET_PSRAM0, &free_va);
    esp_rom_printf("  SOC_MMU_ENTRY_NUM=%d page=%lu KB  PSRAM VA window %08lx-%08lx (%lu pages)\n",
                   SOC_MMU_ENTRY_NUM, H(PAGE / 1024), H(SOC_EXTRAM_LOW), H(SOC_EXTRAM_HIGH - 1),
                   H((SOC_EXTRAM_HIGH - SOC_EXTRAM_LOW) / PAGE));
    esp_rom_printf("  largest free PSRAM VA block: %lu KB\n", H(free_va / 1024));

    uint32_t eA = mmu_ll_get_entry_id(PSRAM_MMU, s_A);
    uint32_t eal = mmu_ll_get_entry_id(PSRAM_MMU, s_alias);
    esp_rom_printf("  entry(A)=%lu raw=%08lx   entry(alias)=%lu raw=%08lx\n",
                   H(eA), H(mmu_ll_read_entry(PSRAM_MMU, eA)), H(eal), H(mmu_ll_read_entry(PSRAM_MMU, eal)));

    /* Clean the alias page out of the cache so no dirty line lands on page B later. */
    esp_cache_msync((void *)s_alias, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    uint32_t before = *(volatile uint32_t *)s_alias;           /* now cached, page A */

    mmu_ll_write_entry(PSRAM_MMU, eal, mmu_val(s_pB), MMU_TARGET_PSRAM0);
    __asm__ volatile("fence" ::: "memory");
    uint32_t no_inval = *(volatile uint32_t *)s_alias;
    esp_cache_msync((void *)s_alias, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    uint32_t after = *(volatile uint32_t *)s_alias;

    esp_rom_printf("    before remap: %08lx | after remap, no invalidate: %08lx | after invalidate: %08lx\n",
                   H(before), H(no_inval), H(after));
    bool remap_ok = before == SENT_A0 && after == SENT_B0;
    chk("remapping one entry A->B shows page B after cache invalidate", remap_ok);
    if (no_inval == SENT_A0) {
        esp_rom_printf("    finding: stale page A data until invalidate -> cache is tagged by CPU address; switch must flush\n");
    } else if (no_inval == SENT_B0) {
        esp_rom_printf("    finding: page B visible without invalidate (line was not cached, or cache tags physical)\n");
    }

    mmu_ll_write_entry(PSRAM_MMU, eal, mmu_val(s_pA), MMU_TARGET_PSRAM0);
    __asm__ volatile("fence" ::: "memory");
    esp_cache_msync((void *)s_alias, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    uint32_t restored = *(volatile uint32_t *)s_alias;
    chk("alias restored to page A", restored == SENT_A0);

    s_res[P5] = (remap_ok && restored == SENT_A0) ? R_PASS : R_FAIL;
}

/* ------------------------------------------------------------------ */
/* P6 — timings                                                        */
/* ------------------------------------------------------------------ */
static void probe_p6(void)
{
    esp_rom_printf("\n== P6: timings (CPU %d MHz, PSRAM per sdkconfig) ==\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    uint32_t t0, t1;

    if (e0 >= 0) {
        t0 = cyc();
        for (int i = 0; i < TIMING_N; i++) {
            pmp_napot(e0, s_A, PAGE, PERM_R);
        }
        t1 = cyc();
        pmp_off(e0);
        print_cycles("PMP entry rewrite (addr + cfg + fence)", t1 - t0, TIMING_N);
    }

    if (s_u_ok && s_psram_denied && e0 >= 0) {
        pmp_napot(e0, s_A, PAGE, PERM_R);
        t0 = cyc();
        uint32_t f0 = run_user(u_fault_loop, s_A);
        t1 = cyc();
        uint32_t base = t1 - t0;
        pmp_off(e0);

        g_storm_limit = FAULT_LOOP_N + 8;
        t0 = cyc();
        uint32_t f1 = run_user(u_fault_loop, s_A);
        t1 = cyc();
        g_storm_limit = 32;
        uint32_t faulting = t1 - t0;
        esp_rom_printf("    fault loop: %lu faults (expect %d), baseline run had %lu\n",
                       H(f1), FAULT_LOOP_N, H(f0));
        if (f1 == FAULT_LOOP_N && faulting > base) {
            print_cycles("U-mode PMP fault round trip (trap+skip+mret)", faulting - base, FAULT_LOOP_N);
        }
    }

    uint32_t eal = mmu_ll_get_entry_id(PSRAM_MMU, s_alias);
    t0 = cyc();
    for (int i = 0; i < TIMING_N; i++) {
        mmu_ll_write_entry(PSRAM_MMU, eal, mmu_val((i & 1) ? s_pA : s_pB), MMU_TARGET_PSRAM0);
    }
    t1 = cyc();
    mmu_ll_write_entry(PSRAM_MMU, eal, mmu_val(s_pA), MMU_TARGET_PSRAM0);
    esp_cache_msync((void *)s_alias, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    print_cycles("cache-MMU entry write (1 x 64 KB page)", t1 - t0, TIMING_N);

    volatile uint32_t *al = (volatile uint32_t *)s_alias;
    for (uint32_t i = 0; i < PAGE / 4; i++) {
        al[i] = i;
    }
    t0 = cyc();
    esp_cache_msync((void *)s_alias, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    t1 = cyc();
    print_cycles("write back + invalidate 64 KB (all dirty)", t1 - t0, 1);

    for (uint32_t i = 0; i < PAGE / 4; i += 16) {
        (void)al[i];
    }
    t0 = cyc();
    esp_cache_msync((void *)s_alias, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    t1 = cyc();
    print_cycles("invalidate 64 KB (clean)", t1 - t0, 1);

    for (uint32_t i = 0; i < PAGE / 4; i++) {
        al[i] = ~i;
    }
    t0 = cyc();
    esp_cache_msync((void *)s_alias, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    mmu_ll_write_entry(PSRAM_MMU, eal, mmu_val(s_pB), MMU_TARGET_PSRAM0);
    __asm__ volatile("fence" ::: "memory");
    t1 = cyc();
    print_cycles("full 64 KB page switch (flush dirty + remap)", t1 - t0, 1);

    mmu_ll_write_entry(PSRAM_MMU, eal, mmu_val(s_pA), MMU_TARGET_PSRAM0);
    esp_cache_msync((void *)s_alias, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    esp_rom_printf("  note: the page A test data was overwritten by the timing runs\n");
    s_res[P6] = R_INFO;
}

/* ------------------------------------------------------------------ */
/* P5d — access through an invalidated MMU entry (runs last)           */
/* ------------------------------------------------------------------ */
static void probe_p5d(void)
{
    esp_rom_printf("\n== P5d: access through an invalidated MMU entry ==\n");
    esp_rom_printf("  If nothing prints after the next line, the access hung the bus: that is the result.\n");
    esp_rom_printf("  A 'Cache error' panic after the probe window closes would also be the result.\n");
    uint32_t e2 = mmu_ll_get_entry_id(PSRAM_MMU, s_alias2);
    esp_cache_msync((void *)s_alias2, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    mmu_ll_set_entry_invalid(PSRAM_MMU, e2);
    __asm__ volatile("fence" ::: "memory");
    esp_rom_printf("  reading %08lx (entry %lu now invalid)...\n", H(s_alias2), H(e2));

    reset_run();
    uint32_t v = *(volatile uint32_t *)s_alias2;
    __asm__ volatile("" ::: "memory");
    uint32_t traps = g_run_traps;

    esp_rom_printf("  returned: value=%08lx traps=%lu\n", H(v), H(traps));
    if (traps) {
        print_trap("trap", &g_first);
        esp_rom_printf("  => synchronous trap on invalid entry (mcause %lu)\n", H(g_first.cause));
    } else {
        esp_rom_printf("  => no synchronous trap: invalid entries cannot be used as page faults\n");
    }

    mmu_ll_write_entry(PSRAM_MMU, e2, mmu_val(s_pB), MMU_TARGET_PSRAM0);
    __asm__ volatile("fence" ::: "memory");
    esp_cache_msync((void *)s_alias2, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    s_res[P5D] = R_INFO;
}

/* ------------------------------------------------------------------ */
/* Core 1 task                                                         */
/* ------------------------------------------------------------------ */
static volatile bool s_done;

static void print_summary(void)
{
    esp_rom_printf("\n================ SUMMARY ================\n");
    for (int i = 0; i < NPROBES; i++) {
        if (i == P5D) {
            continue;
        }
        esp_rom_printf("  %s %s\n", s_names[i], res_str(s_res[i]));
    }
    esp_rom_printf("=========================================\n");
}

static void probe_task(void *arg)
{
    (void)arg;
    esp_rom_printf("\n[probe] running on core %d\n", esp_cpu_get_core_id());

    if (!setup_memory()) {
        esp_rom_printf("[probe] setup failed, no probes run\n");
        s_done = true;
        vTaskDelete(NULL);
        return;
    }

    probe_p0();

    probe_env_enter();
    probe_p4();
    probe_p1();
    probe_p2();
    probe_p3();
    probe_p5();
    probe_p6();
    if (e0 >= 0) {
        pmp_off(e0);
    }
    if (e1 >= 0) {
        pmp_off(e1);
    }
    print_summary();
#if PROBE_RUN_P5D
    probe_p5d();
#endif
    probe_env_exit();

    esp_rom_printf("\n[probe] probe window closed, interrupts back on\n");
    esp_mmu_unmap((void *)s_alias);
    esp_mmu_unmap((void *)s_alias2);
    heap_caps_free(s_buf);
    s_done = true;
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    esp_rom_printf("\n=== BenMMU probe (Stage 0) ===\n");
    esp_rom_printf("chip revision v%d.%d, %d cores, CPU %d MHz, MMU page %lu KB\n",
                   info.revision / 100, info.revision % 100, info.cores,
                   CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, H(PAGE / 1024));

    xTaskCreatePinnedToCore(probe_task, "benmmu_probe", 8192, NULL, configMAX_PRIORITIES - 1, NULL, 1);
    while (!s_done) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_rom_printf("\n=== BenMMU probe finished ===\n");
}
