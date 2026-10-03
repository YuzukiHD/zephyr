/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <offsets.h>
#include <zephyr/toolchain.h>

#ifdef CONFIG_SUN252I_F101_RVV

GTEXT(__soc_save_context)
GTEXT(__soc_restore_context)

/*
 * Whole register stores/loads do not depend on vl and vtype, so the state
 * is the 32 registers plus vstart, vl, vtype and vcsr (vxrm and vxsat).
 * mstatus.VS is always on, see soc_early_init_hook() and arch_new_thread().
 */

/* void __soc_save_context(soc_esf_t *state) */
SECTION_FUNC(exception.other, __soc_save_context)
	csrr t0, vstart
	csrr t1, vl
	csrr t2, vtype
	csrr t3, vcsr
	sw t0, __soc_esf_t_vstart_OFFSET(a0)
	sw t1, __soc_esf_t_vl_OFFSET(a0)
	sw t2, __soc_esf_t_vtype_OFFSET(a0)
	sw t3, __soc_esf_t_vcsr_OFFSET(a0)

	addi t1, a0, __soc_esf_t_v_OFFSET
	csrr t0, vlenb
	slli t0, t0, 3			/* bytes of 8 registers */
	vs8r.v v0, (t1)
	add t1, t1, t0
	vs8r.v v8, (t1)
	add t1, t1, t0
	vs8r.v v16, (t1)
	add t1, t1, t0
	vs8r.v v24, (t1)
	ret

/* void __soc_restore_context(soc_esf_t *state) */
SECTION_FUNC(exception.other, __soc_restore_context)
	addi t1, a0, __soc_esf_t_v_OFFSET
	csrr t0, vlenb
	slli t0, t0, 3
	vl8re8.v v0, (t1)
	add t1, t1, t0
	vl8re8.v v8, (t1)
	add t1, t1, t0
	vl8re8.v v16, (t1)
	add t1, t1, t0
	vl8re8.v v24, (t1)

	lw t1, __soc_esf_t_vl_OFFSET(a0)
	lw t2, __soc_esf_t_vtype_OFFSET(a0)
	lw t3, __soc_esf_t_vcsr_OFFSET(a0)
	lw t0, __soc_esf_t_vstart_OFFSET(a0)
	vsetvl zero, t1, t2		/* sets vl and vtype, clears vstart */
	csrw vcsr, t3
	csrw vstart, t0
	ret

#endif /* CONFIG_SUN252I_F101_RVV */
