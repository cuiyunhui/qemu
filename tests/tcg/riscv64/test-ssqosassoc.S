/*
 * Supervisor-mode QoS association test.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define CSR_SRMCFG_ASSOC       0x182
#define MSTATUS_MPP_MASK       (3 << 11)
#define MSTATUS_MPP_S          (1 << 11)
#define MEDELEG_ECALL_U        (1 << 8)
#define HSTATUS_SPV            (1 << 7)

	.option norvc
	.text
	.global _start
_start:
	lla	t0, machine_trap
	csrw	mtvec, t0

	/* Allow S-mode and U-mode to access the test image. */
	li	t0, -1
	csrw	pmpaddr0, t0
	li	t0, 0x1f
	csrw	pmpcfg0, t0

	lla	t0, supervisor_trap
	csrw	stvec, t0
	li	t0, MEDELEG_ECALL_U
	csrw	medeleg, t0

	/* The supervisor association is disabled after reset. */
	csrr	t0, CSR_SRMCFG_ASSOC
	bnez	t0, fail

	/* Task association: RCID=5, MCID=42. */
	li	t0, 0x002a0005
	csrw	0x181, t0

	/* Supervisor association: RCID=1, MCID=10, all enables set. */
	li	t0, 0x900a1001
	csrw	CSR_SRMCFG_ASSOC, t0
	csrr	t1, CSR_SRMCFG_ASSOC
	bne	t0, t1, fail

	/* Enter U-mode. */
	csrr	t0, mstatus
	li	t1, ~MSTATUS_MPP_MASK
	and	t0, t0, t1
	csrw	mstatus, t0
	lla	t0, user
	csrw	mepc, t0
	mret

user:
	/* Three traps exercise full, RCID-only, and disabled association. */
	ecall
	ecall
	ecall
	li	a0, 0
	j	_exit

supervisor_trap:
	csrr	t0, scause
	li	t1, 8
	bne	t0, t1, fail

	csrr	t0, sepc
	addi	t0, t0, 4
	csrw	sepc, t0

	lla	t0, phase
	lw	t1, 0(t0)
	beqz	t1, rcid_only
	li	t2, 1
	beq	t1, t2, disable_assoc
	li	a0, 0
	ecall

rcid_only:
	/* Keep supervisor RCID, but inherit MCID=42 from srmcfg. */
	li	t2, 0x80001001
	csrw	CSR_SRMCFG_ASSOC, t2
	li	t1, 1
	sw	t1, 0(t0)
	sret

disable_assoc:
	/* Clearing ASSOC_EN restores the task RCID and MCID in S-mode. */
	csrw	CSR_SRMCFG_ASSOC, zero
	li	t1, 2
	sw	t1, 0(t0)
	sret

fail:
	li	a0, 1
	ecall
	j	.

machine_trap:
	csrr	t0, mcause
	li	t1, 9
	beq	t0, t1, enter_vs
	li	t1, 10
	beq	t0, t1, _exit
	li	a0, 1
	j	_exit

enter_vs:
	/* Return first to HS-mode, which will then enter VS-mode. */
	csrr	t0, mstatus
	li	t1, ~MSTATUS_MPP_MASK
	and	t0, t0, t1
	li	t1, MSTATUS_MPP_S
	or	t0, t0, t1
	csrw	mstatus, t0
	lla	t0, host_enter_guest
	csrw	mepc, t0
	mret

host_enter_guest:
	/* SRET with hstatus.SPV=1 and sstatus.SPP=1 enters VS-mode. */
	li	t0, 0x100
	csrs	sstatus, t0
	li	t0, HSTATUS_SPV
	csrs	hstatus, t0
	lla	t0, guest_supervisor
	csrw	sepc, t0
	sret

guest_supervisor:
	/* VS-mode must use srmcfg rather than the host association. */
	ecall
	j	.

_exit:
	lla	a1, semiargs
	li	t0, 0x20026
	sd	t0, 0(a1)
	sd	a0, 8(a1)
	li	a0, 0x20

	.balign	16
	slli	zero, zero, 0x1f
	ebreak
	srai	zero, zero, 0x7
	j	.

	.data
	.balign	16
phase:
	.word	0

	.balign	16
semiargs:
	.space	16
