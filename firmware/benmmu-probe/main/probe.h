#pragma once
#include <stdint.h>

/* Must match the layout in probe_trap.S */
typedef struct {
    uint32_t x[32];     /* x[2] = interrupted sp */
    uint32_t mepc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t mstatus;
} trap_frame_t;

_Static_assert(sizeof(trap_frame_t) == 144, "trap frame layout must match probe_trap.S");

/* probe_trap.S */
extern void probe_trap_vector(void);
extern void probe_run_user(uint32_t fn, uint32_t user_sp, uint32_t arg);
extern void probe_user_exit_stub(void);
extern uint32_t probe_mprv_load(uint32_t addr, uint32_t mpp);
extern void probe_mprv_store(uint32_t addr, uint32_t mpp, uint32_t val);
extern char probe_mprv_load_pc[];
extern char probe_mprv_store_pc[];

/* Called from probe_trap_vector. Returns 0 to resume, 1 to leave user mode. */
int probe_trap_c(trap_frame_t *f);
