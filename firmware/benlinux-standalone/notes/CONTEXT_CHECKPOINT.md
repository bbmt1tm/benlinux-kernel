# BenLinux /bin/sh tick — context checkpoint (for Option B fresh chat)

## Problem
BenLinux (Linux 6.12 NOMMU, M-mode kernel + U-mode userspace) on ESP32-P4 Core 1 via
BenVisor M-mode shim. Froze at "Run /bin/sh".

## Root cause — proven by exhaustion (~12 flashes)
1. Kernel never got ANY timer tick: BenVisor injected line 7 only when `time>=mtimecmp`,
   but IPC mtimecmp stays 0xFFFF... (kernel writes systimer TARGET1 directly, never IPC).
   FIX: BenVisor ISR compares systimer COUNTER (16MHz) vs kernel TARGET1, injects line 7.
2. Line 7 must be VECTORED (SHV=1): SHV=0 lines latch IP but never reach mtvec on this
   P4 CLIC (proven; also triggers cfg corruption 0x48695f44). FIX: line7 SHV=1,
   mtvt[7]=_start_trap, DT rebind hwirq 24->7.
3. Line 7 must be EDGE: kernel handler clears systimer INT_ST, not clicintip[7]; LEVEL
   => stuck-pending livelock. FIX: line7 EDGE. RESULT: cause-7 delivered, scheduler ran,
   RX echo, tx advanced — kernel tick delivery WORKS.
4. THE WALL: BenVisor's heartbeat = line 24 (systimer->INTMTX->CLIC24). In U-mode the P4
   CLIC will NOT latch clicintip[24] from the systimer HW path (proven LEVEL and EDGE,
   identical freeze: ST=0x2, ip=0, isr=0). Software-written clicintip DOES work in U-mode
   (line 7 proves it). Asymmetry: HW-set fails in U-mode, SW-set works.

## Option A (this build) — software self-rearm of line 24
benvisor_asm.S writes clicintip[24]=1 (0x20801060) before mret so line 24 re-fires
without the systimer. KNOWN FLAW: line24 pri 7 re-fires immediately => ~100% Core 1 CPU,
kernel barely runs. PoC only — to see if a shell prompt ever appears.

## Option B (fresh chat) — efficient U-mode-deliverable tick
- B1: CLIC clicintattr mode field — can a line latch in U-mode? We always use mode=11(M).
- B2: CLINT mtimecmp — standard RISC-V machine timer IRQ, not U-gated. Does P4 have it?
  Kernel has native timer-riscv path. The DESIGNED mechanism. STRONGEST candidate.
- B3: route systimer to a different CLIC line not U-gated.

## Key facts/addresses
- Kernel on SD: md5 873c397aa1fdcd1eb139cef15d10267e; _start_trap=0x484c78dc (verified;
  PATCH 10 in restore path, entry unchanged; mret at 0x484c7ae8). App-only builds, NO
  kernel rebuild needed for A. DT rebind lives in esp32p4_dtb.h (regenerated via dtc).
- CLIC ctrl regs: line24=0x20801060, line7=0x2080101C. Bytes: 0=ip,1=ie,2=attr,3=ctl.
  attr bits: bit0=shv, [2:1]=trig(00 level/01 edge), [7:6]=mode(11=M). thresh=0x20800008.
  cliccfg=0x20800000 (healthy 0x7).
- systimer base 0x500E2000: TARGET1_HI 0x24/LO 0x28; UNIT0_OP 0x04 (write 1<<30 latch);
  UNIT0_VAL_HI 0x40/LO 0x44; COMP1_LOAD 0x54; INT_ENA 0x64; INT_CLR 0x6C (bit1=alarm1);
  CONF 0x00 (TARGET1_EN bit23). CSR time=360MHz (clocksource); systimer=16MHz (alarm).
- Tools: riscv64-unknown-elf-as/objdump (-march=rv32ima_zicsr -mabi=ilp32), dtc.
- Trees: pristine /home/claude/project; kernel CI /tmp/cibuild/benlinux-kernel-main;
  SD kernel /tmp/kimg2/Image.
