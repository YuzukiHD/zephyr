/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RISC-V Sv32 memory management definitions
 *
 * The kernel runs in machine mode and keeps its code untranslated; loads and
 * stores go through the Sv32 page tables because mstatus.MPRV is set and
 * mstatus.MPP is U (see arch/riscv/core/mmu.c). That is why the leaf entries
 * are user pages.
 */

#ifndef ZEPHYR_INCLUDE_ARCH_RISCV_MM_H_
#define ZEPHYR_INCLUDE_ARCH_RISCV_MM_H_

#include <zephyr/sys/util.h>

/* Sv32 page table entry */
#define RISCV_PTE_V		BIT(0)
#define RISCV_PTE_R		BIT(1)
#define RISCV_PTE_W		BIT(2)
#define RISCV_PTE_X		BIT(3)
#define RISCV_PTE_U		BIT(4)
#define RISCV_PTE_G		BIT(5)
#define RISCV_PTE_A		BIT(6)
#define RISCV_PTE_D		BIT(7)
/* software bit: the entry is not valid, its PPN field holds a backing store location */
#define RISCV_PTE_PAGED_OUT	BIT(8)
#define RISCV_PTE_PPN_SHIFT	10
#define RISCV_PTE_PERM_MASK	(RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X | RISCV_PTE_U)

#define RISCV_SV32_MEGAPAGE	(4U * 1024 * 1024)

/* Definitions used by arch_page_info_get() */
#define ARCH_DATA_PAGE_LOADED		BIT(0)
#define ARCH_DATA_PAGE_ACCESSED		BIT(1)
#define ARCH_DATA_PAGE_DIRTY		BIT(2)
#define ARCH_DATA_PAGE_NOT_MAPPED	BIT(3)

/*
 * Special unpaged "location" tags: the highest page numbers that fit the PPN
 * field, unlikely to be the location of a page in a backing store.
 */
#define ARCH_UNPAGED_ANON_ZERO		0xfffff000UL
#define ARCH_UNPAGED_ANON_UNINIT	0xffffe000UL

#ifndef _ASMLANGUAGE

#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/sys_io.h>

struct arch_esf;

/** A physical range mapped 1:1 at boot, in whole 4 MiB pages (peripherals) */
struct riscv_mmu_region {
	const char *name;
	uintptr_t base;
	size_t size;
};

/*
 * Registers of the peripherals that sit inside the core (CLINT, PLIC) answer
 * to machine mode only; an access that goes through the page tables is made
 * with the privilege of user mode and reads as all ones. These accessors make
 * the access untranslated: mstatus.MPRV is off for the one load or store, and
 * is put back to what it was. An interrupt in between returns with MPRV as it
 * was saved, which is off, and the sequence goes on.
 */
#ifdef CONFIG_RISCV_MMU
static inline uint32_t z_riscv_mmode_read32(mem_addr_t addr)
{
	unsigned long mprv = 0x00020000UL; /* MSTATUS_MPRV */
	unsigned long saved;
	uint32_t val;

	__asm__ volatile("csrrc %[s], mstatus, %[m]\n"
			 "lw %[v], 0(%[a])\n"
			 "and %[s], %[s], %[m]\n"
			 "csrs mstatus, %[s]"
			 : [s] "=&r"(saved), [v] "=&r"(val)
			 : [m] "r"(mprv), [a] "r"(addr)
			 : "memory");

	return val;
}

static inline void z_riscv_mmode_write32(uint32_t val, mem_addr_t addr)
{
	unsigned long mprv = 0x00020000UL; /* MSTATUS_MPRV */
	unsigned long saved;

	__asm__ volatile("csrrc %[s], mstatus, %[m]\n"
			 "sw %[v], 0(%[a])\n"
			 "and %[s], %[s], %[m]\n"
			 "csrs mstatus, %[s]"
			 : [s] "=&r"(saved)
			 : [m] "r"(mprv), [v] "r"(val), [a] "r"(addr)
			 : "memory");
}
#else
#define z_riscv_mmode_read32(addr)		sys_read32(addr)
#define z_riscv_mmode_write32(val, addr)	sys_write32(val, addr)
#endif

/** Provided by the SoC */
extern const struct riscv_mmu_region riscv_mmu_regions[];
extern const unsigned int riscv_mmu_regions_count;

#define RISCV_MMU_REGION(_name, _base, _size)				\
	{ .name = _name, .base = _base, .size = _size }

/** Build the page tables, open the PMP and turn the translation of loads and stores on */
void z_riscv_mm_init(void);

/**
 * Handle a load/store page fault (mcause 13 / 15).
 *
 * Emulates the accessed and dirty bits and pages in demand paged memory.
 *
 * @retval true the access is to be retried
 * @retval false the fault is a real one
 */
bool z_riscv_mm_page_fault(struct arch_esf *esf, unsigned long mcause, unsigned long addr);

#endif /* _ASMLANGUAGE */

#endif /* ZEPHYR_INCLUDE_ARCH_RISCV_MM_H_ */
