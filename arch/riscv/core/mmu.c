/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Sv32 memory management for a machine mode kernel.
 *
 * The kernel never leaves machine mode, but its loads and stores are
 * translated: mstatus.MPRV is set and mstatus.MPP is U, so the data accesses
 * are made as if by user mode code, and the leaf entries are user pages. The
 * fetch of instructions is not translated.
 *
 *  - A trap sets MPP to M, which switches the translation off until the trap
 *    returns: mret puts MPP back to U and leaves MPRV alone. The trap entry
 *    and exit code and the interrupt handlers therefore see physical
 *    addresses, and everything they touch is mapped 1:1.
 *  - The page fault handler switches the translation on again for its own
 *    run, because the backing store fills a frame through the scratch page.
 *  - z_riscv_switch() does the same for a thread that is resumed: another
 *    thread's trap may have left MPP at M.
 *
 * The image and the peripherals are mapped 1:1 at boot, a pool of second
 * level tables covers the whole kernel address space so that arch_mem_map()
 * never allocates. The hardware does not update the accessed and dirty bits,
 * an access with A (or, for a store, D) clear raises a page fault: the bits
 * are emulated in z_riscv_mm_page_fault(), which gives the eviction
 * algorithms what they need.
 */

#include <errno.h>
#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/kernel/mm/demand_paging.h>
#include <zephyr/sys/printk.h>
#include <zephyr/arch/riscv/csr.h>
#include <zephyr/arch/riscv/mm.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/sys/util.h>
#include <kernel_arch_func.h>
#include <kernel_arch_interface.h>
#include <kernel_internal.h>
#include <mmu.h>

#define PAGE_SIZE	CONFIG_MMU_PAGE_SIZE
#define PTES		1024U
#define ROOT_SHIFT	22U

#define VM_START	ROUND_DOWN(CONFIG_KERNEL_VM_BASE, RISCV_SV32_MEGAPAGE)
#define VM_END		ROUND_UP(CONFIG_KERNEL_VM_BASE + CONFIG_KERNEL_VM_SIZE, RISCV_SV32_MEGAPAGE)
#define L2_TABLES	((VM_END - VM_START) / RISCV_SV32_MEGAPAGE)

#define PMP_ALL_ADDR	0x3fffffffUL	/* NAPOT, the whole 34 bit address space */
#define PMP_ALL_CFG	0x1fUL		/* NAPOT, R, W, X */

#define SATP_SV32	BIT(31)

static uint32_t root_table[PTES] __aligned(4096);
static uint32_t l2_tables[L2_TABLES][PTES] __aligned(4096);

static inline uint32_t pte_ppn(uintptr_t phys)
{
	return (uint32_t)(phys >> 12) << RISCV_PTE_PPN_SHIFT;
}

static inline uintptr_t pte_phys(uint32_t pte)
{
	return (uintptr_t)(pte >> RISCV_PTE_PPN_SHIFT) << 12;
}

static inline bool pte_is_table(uint32_t pte)
{
	return (pte & RISCV_PTE_V) != 0U &&
	       (pte & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) == 0U;
}

static inline void tlb_flush(void)
{
	__asm__ volatile("sfence.vma" ::: "memory");
}

/* The page table walker reads memory, not the data cache */
static inline void pte_sync(uint32_t *pte)
{
	sys_cache_data_flush_range(pte, sizeof(*pte));
	__asm__ volatile("fence" ::: "memory");
}

/* The second level entry of an address in the kernel address space, NULL outside of it */
static uint32_t *leaf_pte(uintptr_t va)
{
	uint32_t root;

	if (va < VM_START || va >= VM_END) {
		return NULL;
	}
	root = root_table[va >> ROOT_SHIFT];
	if (!pte_is_table(root)) {
		return NULL;
	}

	return &((uint32_t *)pte_phys(root))[(va >> 12) & (PTES - 1U)];
}

static uint32_t make_leaf(uintptr_t phys, uint32_t flags)
{
	uint32_t pte = RISCV_PTE_R | RISCV_PTE_U;

	if ((flags & K_MEM_PERM_RW) != 0U) {
		pte |= RISCV_PTE_W;
	}
	if ((flags & K_MEM_MAP_UNPAGED) != 0U) {
		/* not present: the location of the page is kept where the frame would be */
		return pte_ppn(phys) | RISCV_PTE_PAGED_OUT | pte;
	}
	/* mapped for good: accessed and, if writable, dirty from the start */
	pte |= RISCV_PTE_V | RISCV_PTE_A;
	if ((flags & K_MEM_PERM_RW) != 0U) {
		pte |= RISCV_PTE_D;
	}

	return pte_ppn(phys) | pte;
}

/* the part of the image that is mapped by 4 MiB entries, for the boot message */
static size_t image_mega;

void z_riscv_mm_init(void)
{
	uintptr_t va, mega_start, mega_end;

	/* second level tables for the whole kernel address space */
	for (unsigned int i = 0; i < L2_TABLES; i++) {
		root_table[(VM_START >> ROOT_SHIFT) + i] = pte_ppn((uintptr_t)l2_tables[i]) | RISCV_PTE_V;
	}

	/*
	 * The image. The 4 MiB blocks that it fills completely are one entry each: the data of the
	 * programs is spread over megabytes, and with 4 KiB pages it does not fit the TLB.
	 * The start of the first block is RAM as well.
	 */
	mega_start = ROUND_DOWN((uintptr_t)z_mapped_start, RISCV_SV32_MEGAPAGE);
	mega_end = mega_start;
	while (mega_end + RISCV_SV32_MEGAPAGE <= (uintptr_t)z_mapped_end) {
		root_table[mega_end >> ROOT_SHIFT] = pte_ppn(mega_end) | RISCV_PTE_V |
			RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_U | RISCV_PTE_A | RISCV_PTE_D;
		mega_end += RISCV_SV32_MEGAPAGE;
	}
	for (va = (uintptr_t)z_mapped_start; va < (uintptr_t)z_mapped_end; va += PAGE_SIZE) {
		if (va < mega_start || va >= mega_end) {
			*leaf_pte(va) = make_leaf(va, K_MEM_PERM_RW);
		}
	}
	image_mega = mega_end - mega_start;

	/* the peripherals */
	for (unsigned int i = 0; i < riscv_mmu_regions_count; i++) {
		const struct riscv_mmu_region *r = &riscv_mmu_regions[i];

		__ASSERT(r->base % RISCV_SV32_MEGAPAGE == 0U && r->size % RISCV_SV32_MEGAPAGE == 0U,
			 "region %s is not made of 4 MiB pages", r->name);
		for (uintptr_t off = 0; off < r->size; off += RISCV_SV32_MEGAPAGE) {
			root_table[(r->base + off) >> ROOT_SHIFT] =
				pte_ppn(r->base + off) | RISCV_PTE_V | RISCV_PTE_R | RISCV_PTE_W |
				RISCV_PTE_U | RISCV_PTE_A | RISCV_PTE_D;
		}
	}
	sys_cache_data_flush_all();

	/* translated accesses are checked by the PMP, one entry lets all of them through */
	csr_write(pmpaddr0, PMP_ALL_ADDR);
	csr_write(pmpcfg0, PMP_ALL_CFG);

	csr_write(satp, SATP_SV32 | ((uintptr_t)root_table >> 12));
	tlb_flush();

	/* from here on the loads and stores of this code are translated */
	csr_clear(mstatus, MSTATUS_MPP);
	csr_set(mstatus, MSTATUS_MPRV);
}

void arch_mem_map(void *virt, uintptr_t phys, size_t size, uint32_t flags)
{
	uintptr_t va = (uintptr_t)virt;

	__ASSERT(va % PAGE_SIZE == 0U && size % PAGE_SIZE == 0U, "unaligned mapping");

	for (size_t off = 0; off < size; off += PAGE_SIZE) {
		uint32_t *pte = leaf_pte(va + off);

		__ASSERT(pte != NULL, "%#lx is outside of the kernel address space", va + off);
		*pte = make_leaf(phys + off, flags);
		pte_sync(pte);
	}
	tlb_flush();
}

void arch_mem_unmap(void *addr, size_t size)
{
	uintptr_t va = (uintptr_t)addr;

	__ASSERT(va % PAGE_SIZE == 0U && size % PAGE_SIZE == 0U, "unaligned mapping");

	for (size_t off = 0; off < size; off += PAGE_SIZE) {
		uint32_t *pte = leaf_pte(va + off);

		__ASSERT(pte != NULL, "%#lx is outside of the kernel address space", va + off);
		*pte = 0U;
		pte_sync(pte);
	}
	tlb_flush();
}

int arch_page_phys_get(void *virt, uintptr_t *phys)
{
	uintptr_t va = (uintptr_t)virt;
	uint32_t root = root_table[va >> ROOT_SHIFT], *pte;

	/* a 4 MiB entry: a block of the image or the registers of peripherals */
	if ((root & RISCV_PTE_V) != 0U && !pte_is_table(root)) {
		if (phys != NULL) {
			*phys = pte_phys(root) | (va & (RISCV_SV32_MEGAPAGE - 1U));
		}
		return 0;
	}

	if (va >= VM_START && va < VM_END) {
		pte = leaf_pte(va);
		if (pte == NULL || (*pte & RISCV_PTE_V) == 0U) {
			return -EFAULT;
		}
		if (phys != NULL) {
			*phys = pte_phys(*pte) | (va & (PAGE_SIZE - 1U));
		}
		return 0;
	}

	return -EFAULT;
}

#ifdef CONFIG_DEMAND_PAGING

void arch_mem_page_out(void *addr, uintptr_t location)
{
	uint32_t *pte = leaf_pte((uintptr_t)addr);

	__ASSERT(pte != NULL, "");
	/* not present for the hardware, the location where the frame was; the permissions stay */
	*pte = pte_ppn(location) | RISCV_PTE_PAGED_OUT | (*pte & RISCV_PTE_PERM_MASK);
	pte_sync(pte);
	tlb_flush();
}

void arch_mem_page_in(void *addr, uintptr_t phys)
{
	uint32_t *pte = leaf_pte((uintptr_t)addr);

	__ASSERT(pte != NULL, "");
	/* clean and not accessed: the first access faults, which is how both are tracked */
	*pte = pte_ppn(phys) | RISCV_PTE_V | (*pte & RISCV_PTE_PERM_MASK);
	pte_sync(pte);
	tlb_flush();
}

enum arch_page_location arch_page_location_get(void *addr, uintptr_t *location)
{
	uint32_t *pte = leaf_pte((uintptr_t)addr);

	if (pte == NULL) {
		return ARCH_PAGE_LOCATION_BAD;
	}
	if ((*pte & RISCV_PTE_V) != 0U) {
		*location = pte_phys(*pte);
		return ARCH_PAGE_LOCATION_PAGED_IN;
	}
	if ((*pte & RISCV_PTE_PAGED_OUT) != 0U) {
		*location = pte_phys(*pte);
		return ARCH_PAGE_LOCATION_PAGED_OUT;
	}

	return ARCH_PAGE_LOCATION_BAD;
}

uintptr_t arch_page_info_get(void *addr, uintptr_t *phys, bool clear_accessed)
{
	uint32_t *pte = leaf_pte((uintptr_t)addr);
	uintptr_t status = 0;

	if (pte == NULL || (*pte & (RISCV_PTE_V | RISCV_PTE_PAGED_OUT)) == 0U) {
		return ARCH_DATA_PAGE_NOT_MAPPED;
	}
	if (phys != NULL) {
		*phys = pte_phys(*pte);
	}
	if ((*pte & RISCV_PTE_V) == 0U) {
		return status;
	}

	status |= ARCH_DATA_PAGE_LOADED;
	if ((*pte & RISCV_PTE_A) != 0U) {
		status |= ARCH_DATA_PAGE_ACCESSED;
	}
	if ((*pte & RISCV_PTE_D) != 0U) {
		status |= ARCH_DATA_PAGE_DIRTY;
	}
	if (clear_accessed) {
		*pte &= ~RISCV_PTE_A;
		pte_sync(pte);
		tlb_flush();
	}

	return status;
}

void arch_mem_scratch(uintptr_t phys)
{
	uint32_t *pte = leaf_pte((uintptr_t)K_MEM_SCRATCH_PAGE);

	__ASSERT(pte != NULL, "");
	*pte = make_leaf(phys, K_MEM_PERM_RW);
	pte_sync(pte);
	tlb_flush();
}

static bool page_in(uintptr_t va, const struct arch_esf *esf)
{
	bool irq_on = (esf->mstatus & MSTATUS_MPIE_EN) != 0U;
	bool ok;

	/*
	 * The handler runs untranslated, the backing store fills the frame
	 * through the scratch page: translate its accesses from here on.
	 */
	csr_clear(mstatus, MSTATUS_MPP);

	/* k_mem_page_fault() wants interrupts on if they were when the fault happened */
	if (irq_on) {
		arch_irq_unlock(MSTATUS_IEN);
	}
	ok = k_mem_page_fault((void *)va);
	(void)arch_irq_lock();

	/* the way out of a trap that did not resolve the fault is untranslated again */
	csr_set(mstatus, MSTATUS_MPP);

	return ok;
}

#endif /* CONFIG_DEMAND_PAGING */

bool z_riscv_mm_page_fault(struct arch_esf *esf, unsigned long mcause, unsigned long addr)
{
	uintptr_t va = ROUND_DOWN(addr, PAGE_SIZE);
	bool store = (mcause == RISCV_EXC_STORE_PAGE_FAULT);
	uint32_t *pte = leaf_pte(va);
	uint32_t entry;

	ARG_UNUSED(esf);

	if (pte == NULL) {
		return false;
	}
	entry = *pte;

	if ((entry & RISCV_PTE_V) != 0U) {
		/*
		 * The page is there: the hardware wants the accessed bit (and the
		 * dirty bit for a store) set, which is what is being tracked.
		 */
		uint32_t want = RISCV_PTE_A | (store ? RISCV_PTE_D : 0U);

		if ((entry & RISCV_PTE_R) == 0U || (store && (entry & RISCV_PTE_W) == 0U) ||
		    (entry & want) == want) {
			return false;
		}
		*pte = entry | want;
		pte_sync(pte);
		tlb_flush();
#ifdef CONFIG_EVICTION_TRACKING
		k_mem_paging_eviction_accessed(pte_phys(entry));
#endif
		return true;
	}

#ifdef CONFIG_DEMAND_PAGING
	if ((entry & RISCV_PTE_PAGED_OUT) != 0U) {
		return page_in(va, esf);
	}
#endif

	return false;
}

#ifdef CONFIG_RISCV_MMU_BOOT_INFO
static int mmu_info(void)
{
	unsigned int mapped = 0, paged_out = 0;

	for (unsigned int i = 0; i < L2_TABLES * PTES; i++) {
		uint32_t pte = l2_tables[i / PTES][i % PTES];

		if ((pte & RISCV_PTE_V) != 0U) {
			mapped++;
		} else if ((pte & RISCV_PTE_PAGED_OUT) != 0U) {
			paged_out++;
		}
	}

	printk("mmu: Sv32, satp %#lx (root table %p), data accesses translated "
	       "(MPRV, MPP=U), instruction fetch is not\n",
	       (unsigned long)csr_read(satp), root_table);
	printk("mmu: 1:1 image     %p-%p, %zu KiB, %zu MiB in 4 MiB pages, the rest 4 KiB pages\n",
	       z_mapped_start, z_mapped_end, (size_t)(z_mapped_end - z_mapped_start) / 1024,
	       image_mega / (1024 * 1024));
	for (unsigned int i = 0; i < riscv_mmu_regions_count; i++) {
		const struct riscv_mmu_region *r = &riscv_mmu_regions[i];

		printk("mmu: 1:1 %-9s 0x%08lx-0x%08lx, 4 MiB pages\n", r->name,
		       (unsigned long)r->base, (unsigned long)(r->base + r->size - 1));
	}
	printk("mmu: kernel VM     0x%08lx-0x%08lx, %u KiB, %lu second level tables (%lu KiB)\n",
	       (unsigned long)CONFIG_KERNEL_VM_BASE,
	       (unsigned long)(CONFIG_KERNEL_VM_BASE + CONFIG_KERNEL_VM_SIZE - 1),
	       (unsigned int)(CONFIG_KERNEL_VM_SIZE / 1024), (unsigned long)L2_TABLES,
	       (unsigned long)L2_TABLES * 4UL);
	printk("mmu: %u pages mapped, %u paged out, %zu KiB of page frames free\n", mapped,
	       paged_out, k_mem_free_get() / 1024);
#ifdef CONFIG_DEMAND_PAGING
	printk("mmu: demand paging, A/D bits emulated%s%s\n",
	       IS_ENABLED(CONFIG_EVICTION_LRU) ? ", LRU eviction" : "",
	       IS_ENABLED(CONFIG_BACKING_STORE_FS) ? ", file backing store" : "");
#else
	printk("mmu: no demand paging\n");
#endif

	return 0;
}

SYS_INIT(mmu_info, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif /* CONFIG_RISCV_MMU_BOOT_INFO */
