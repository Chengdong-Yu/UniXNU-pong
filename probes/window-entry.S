// SPDX-License-Identifier: BSD-2-Clause-Patent
.section .text.entry,"ax"
.global _start
_start:
  msr daifset, #15
  msr spsel, #1
  msr tpidr_el1, x0
  adr x1, probe_vectors
  msr vbar_el1, x1
  isb
  // Use a private stack in the window; do not rely on the firmware's SP.
  ldr x1, [x0, #16]
  ldr x2, [x0, #24]
  add x1, x1, x2
  sub x1, x1, #16
  mov sp, x1
  bl probe_main
1:
  wfe
  b 1b

// A fault after firmware exit must not fall into the old UEFI exception path.
.balign 2048
probe_vectors:
.rept 16
  b probe_fault
  .balign 128
.endr
probe_fault:
  mrs x0, esr_el1
  mrs x1, far_el1
  mrs x2, elr_el1
  mrs x3, tpidr_el1
  ldr x4, [x3, #16]
  ldr x5, [x3, #24]
  add x4, x4, x5
  sub x4, x4, #16
  mov sp, x4
  bl probe_exception
2:
  wfe
  b 2b
