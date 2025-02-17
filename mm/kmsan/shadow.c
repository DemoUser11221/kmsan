// SPDX-License-Identifier: GPL-2.0
/*
 * KMSAN shadow implementation.
 *
 * Copyright (C) 2017-2021 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */

#include <asm/cpu_entry_area.h>
#include <asm/page.h>
#include <asm/pgtable_64_types.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <linux/memblock.h>
#include <linux/mm_types.h>
#include <linux/percpu-defs.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stddef.h>

#include "kmsan.h"
#include "shadow.h"

#define shadow_page_for(page) ((page)->kmsan_shadow)

#define origin_page_for(page) ((page)->kmsan_origin)

static void *shadow_ptr_for(struct page *page)
{
	return page_address(shadow_page_for(page));
}

static void *origin_ptr_for(struct page *page)
{
	return page_address(origin_page_for(page));
}

bool page_has_metadata(struct page *page)
{
	return shadow_page_for(page) && origin_page_for(page);
}

void set_no_shadow_origin_page(struct page *page)
{
	shadow_page_for(page) = NULL;
	origin_page_for(page) = NULL;
}

DEFINE_PER_CPU(char[CPU_ENTRY_AREA_SIZE], cpu_entry_area_shadow);
DEFINE_PER_CPU(char[CPU_ENTRY_AREA_SIZE], cpu_entry_area_origin);

/*
 * Dummy load and store pages to be used when the real metadata is unavailable.
 * There are separate pages for loads and stores, so that every load returns a
 * zero, and every store doesn't affect other loads.
 */
char dummy_load_page[PAGE_SIZE] __aligned(PAGE_SIZE);
char dummy_store_page[PAGE_SIZE] __aligned(PAGE_SIZE);

/*
 * Taken from arch/x86/mm/physaddr.h to avoid using an instrumented version.
 */
static int kmsan_phys_addr_valid(unsigned long addr)
{
	if (IS_ENABLED(CONFIG_PHYS_ADDR_T_64BIT))
		return !(addr >> boot_cpu_data.x86_phys_bits);
	else
		return 1;
}

/*
 * Taken from arch/x86/mm/physaddr.c to avoid using an instrumented version.
 */
static bool kmsan_virt_addr_valid(void *addr)
{
	unsigned long x = (unsigned long)addr;
	unsigned long y = x - __START_KERNEL_map;

	/* use the carry flag to determine if x was < __START_KERNEL_map */
	if (unlikely(x > y)) {
		x = y + phys_base;

		if (y >= KERNEL_IMAGE_SIZE)
			return false;
	} else {
		x = y + (__START_KERNEL_map - PAGE_OFFSET);

		/* carry flag will be set if starting x was >= PAGE_OFFSET */
		if ((x > y) || !kmsan_phys_addr_valid(x))
			return false;
	}

	return pfn_valid(x >> PAGE_SHIFT);
}

static unsigned long vmalloc_meta(void *addr, bool is_origin)
{
	unsigned long addr64 = (unsigned long)addr, off;

	BUG_ON(is_origin && !IS_ALIGNED(addr64, KMSAN_ORIGIN_SIZE));
	if (kmsan_internal_is_vmalloc_addr(addr)) {
		off = addr64 - VMALLOC_START;
		return off + (is_origin ? KMSAN_VMALLOC_ORIGIN_START :
						KMSAN_VMALLOC_SHADOW_START);
	}
	if (kmsan_internal_is_module_addr(addr)) {
		off = addr64 - MODULES_VADDR;
		return off + (is_origin ? KMSAN_MODULES_ORIGIN_START :
						KMSAN_MODULES_SHADOW_START);
	}
	return 0;
}

static void *get_cea_meta_or_null(void *addr, bool is_origin)
{
	int cpu = smp_processor_id();
	char *metadata_array;
	int off;

	if (((u64)addr < CPU_ENTRY_AREA_BASE) ||
	    ((u64)addr >= (CPU_ENTRY_AREA_BASE + CPU_ENTRY_AREA_MAP_SIZE)))
		return NULL;
	off = (char *)addr - (char *)get_cpu_entry_area(cpu);
	if ((off < 0) || (off >= CPU_ENTRY_AREA_SIZE))
		return NULL;
	metadata_array =
		is_origin ? cpu_entry_area_origin : cpu_entry_area_shadow;
	return &per_cpu(metadata_array[off], cpu);
}

static struct page *virt_to_page_or_null(void *vaddr)
{
	if (kmsan_virt_addr_valid(vaddr))
		return virt_to_page(vaddr);
	else
		return NULL;
}

struct shadow_origin_ptr kmsan_get_shadow_origin_ptr(void *address, u64 size,
						     bool store)
{
	struct shadow_origin_ptr ret;
	void *shadow;

	/*
	 * Even if we redirect this memory access to the dummy page, it will
	 * go out of bounds.
	 */
	BUG_ON(size > PAGE_SIZE);

	if (!kmsan_ready || kmsan_in_runtime())
		goto return_dummy;

	BUG_ON(!kmsan_metadata_is_contiguous(address, size));
	shadow = kmsan_get_metadata(address, KMSAN_META_SHADOW);
	if (!shadow)
		goto return_dummy;

	ret.shadow = shadow;
	ret.origin = kmsan_get_metadata(address, KMSAN_META_ORIGIN);
	return ret;

return_dummy:
	if (store) {
		/* Ignore this store. */
		ret.shadow = dummy_store_page;
		ret.origin = dummy_store_page;
	} else {
		/* This load will return zero. */
		ret.shadow = dummy_load_page;
		ret.origin = dummy_load_page;
	}
	return ret;
}

/*
 * Obtain the shadow or origin pointer for the given address, or NULL if there's
 * none. The caller must check the return value for being non-NULL if needed.
 * The return value of this function should not depend on whether we're in the
 * runtime or not.
 */
void *kmsan_get_metadata(void *address, bool is_origin)
{
	u64 addr = (u64)address, pad, off;
	struct page *page;
	void *ret;

	if (is_origin && !IS_ALIGNED(addr, KMSAN_ORIGIN_SIZE)) {
		pad = addr % KMSAN_ORIGIN_SIZE;
		addr -= pad;
	}
	address = (void *)addr;
	if (kmsan_internal_is_vmalloc_addr(address) ||
	    kmsan_internal_is_module_addr(address))
		return (void *)vmalloc_meta(address, is_origin);

	ret = get_cea_meta_or_null(address, is_origin);
	if (ret)
		return ret;

	page = virt_to_page_or_null(address);
	if (!page)
		return NULL;
	if (!page_has_metadata(page))
		return NULL;
	off = addr % PAGE_SIZE;

	ret = (is_origin ? origin_ptr_for(page) : shadow_ptr_for(page)) + off;
	return ret;
}

/* Allocate metadata for pages allocated at boot time. */
void __init kmsan_init_alloc_meta_for_range(void *start, void *end)
{
	struct page *shadow_p, *origin_p;
	void *shadow, *origin;
	struct page *page;
	u64 addr, size;

	start = (void *)ALIGN_DOWN((u64)start, PAGE_SIZE);
	size = ALIGN((u64)end - (u64)start, PAGE_SIZE);
	shadow = memblock_alloc(size, PAGE_SIZE);
	origin = memblock_alloc(size, PAGE_SIZE);
	for (addr = 0; addr < size; addr += PAGE_SIZE) {
		page = virt_to_page_or_null((char *)start + addr);
		shadow_p = virt_to_page_or_null((char *)shadow + addr);
		set_no_shadow_origin_page(shadow_p);
		shadow_page_for(page) = shadow_p;
		origin_p = virt_to_page_or_null((char *)origin + addr);
		set_no_shadow_origin_page(origin_p);
		origin_page_for(page) = origin_p;
	}
}

/* Called from mm/memory.c */
void kmsan_copy_page_meta(struct page *dst, struct page *src)
{
	unsigned long irq_flags;

	if (!kmsan_ready || kmsan_in_runtime())
		return;
	if (!dst || !page_has_metadata(dst))
		return;
	if (!src || !page_has_metadata(src)) {
		kmsan_internal_unpoison_memory(page_address(dst), PAGE_SIZE,
					       /*checked*/ false);
		return;
	}

	irq_flags = kmsan_enter_runtime();
	__memcpy(shadow_ptr_for(dst), shadow_ptr_for(src), PAGE_SIZE);
	__memcpy(origin_ptr_for(dst), origin_ptr_for(src), PAGE_SIZE);
	kmsan_leave_runtime(irq_flags);
}

/* Called from mm/page_alloc.c */
void kmsan_alloc_page(struct page *page, unsigned int order, gfp_t flags)
{
	bool initialized = (flags & __GFP_ZERO) || !kmsan_ready;
	struct page *shadow, *origin;
	depot_stack_handle_t handle;
	unsigned long irq_flags;
	int pages = 1 << order;
	int i;

	if (!page)
		return;

	shadow = shadow_page_for(page);
	origin = origin_page_for(page);

	if (initialized) {
		__memset(page_address(shadow), 0, PAGE_SIZE * pages);
		__memset(page_address(origin), 0, PAGE_SIZE * pages);
		return;
	}

	/* Zero pages allocated by the runtime should also be initialized. */
	if (kmsan_in_runtime())
		return;

	__memset(page_address(shadow), -1, PAGE_SIZE * pages);
	irq_flags = kmsan_enter_runtime();
	handle = kmsan_save_stack_with_flags(flags, /*extra_bits*/ 0);
	kmsan_leave_runtime(irq_flags);
	/*
	 * Addresses are page-aligned, pages are contiguous, so it's ok
	 * to just fill the origin pages with |handle|.
	 */
	for (i = 0; i < PAGE_SIZE * pages / sizeof(handle); i++)
		((depot_stack_handle_t *)page_address(origin))[i] = handle;
}

/* Called from mm/page_alloc.c */
void kmsan_free_page(struct page *page, unsigned int order)
{
	return; // really nothing to do here. Could rewrite shadow instead.
}

/* Called from mm/vmalloc.c */
void kmsan_vmap_pages_range_noflush(unsigned long start, unsigned long end,
				    pgprot_t prot, struct page **pages,
				    unsigned int page_shift)
{
	unsigned long shadow_start, origin_start, shadow_end, origin_end;
	struct page **s_pages, **o_pages;
	unsigned long irq_flags;
	int nr, i, mapped;

	if (!kmsan_ready)
		return;

	shadow_start = vmalloc_meta((void *)start, KMSAN_META_SHADOW);
	shadow_end = vmalloc_meta((void *)end, KMSAN_META_SHADOW);
	if (!shadow_start)
		return;

	nr = (end - start) / PAGE_SIZE;
	s_pages = kcalloc(nr, sizeof(struct page *), GFP_KERNEL);
	o_pages = kcalloc(nr, sizeof(struct page *), GFP_KERNEL);
	if (!s_pages || !o_pages)
		goto ret;
	for (i = 0; i < nr; i++) {
		s_pages[i] = shadow_page_for(pages[i]);
		o_pages[i] = origin_page_for(pages[i]);
	}
	prot = __pgprot(pgprot_val(prot) | _PAGE_NX);
	prot = PAGE_KERNEL;

	origin_start = vmalloc_meta((void *)start, KMSAN_META_ORIGIN);
	origin_end = vmalloc_meta((void *)end, KMSAN_META_ORIGIN);
	irq_flags = kmsan_enter_runtime();
	mapped = __vmap_pages_range_noflush(shadow_start, shadow_end, prot,
					    s_pages, page_shift);
	BUG_ON(mapped);
	mapped = __vmap_pages_range_noflush(origin_start, origin_end, prot,
					    o_pages, page_shift);
	BUG_ON(mapped);
	kmsan_leave_runtime(irq_flags);
	flush_tlb_kernel_range(shadow_start, shadow_end);
	flush_tlb_kernel_range(origin_start, origin_end);
	flush_cache_vmap(shadow_start, shadow_end);
	flush_cache_vmap(origin_start, origin_end);

ret:
	kfree(s_pages);
	kfree(o_pages);
}

void kmsan_setup_meta(struct page *page, struct page *shadow,
		      struct page *origin, int order)
{
	int i;

	for (i = 0; i < (1 << order); i++) {
		set_no_shadow_origin_page(&shadow[i]);
		set_no_shadow_origin_page(&origin[i]);
		shadow_page_for(&page[i]) = &shadow[i];
		origin_page_for(&page[i]) = &origin[i];
	}
}
