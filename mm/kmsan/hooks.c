// SPDX-License-Identifier: GPL-2.0
/*
 * KMSAN hooks for kernel subsystems.
 *
 * These functions handle creation of KMSAN metadata for memory allocations.
 *
 * Copyright (C) 2018-2021 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */

#include <asm/cacheflush.h>
#include <linux/dma-direction.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "../slab.h"
#include "kmsan.h"

/*
 * Instrumented functions shouldn't be called under
 * kmsan_enter_runtime()/kmsan_leave_runtime(), because this will lead to
 * skipping effects of functions like memset() inside instrumented code.
 */

/* Called from kernel/fork.c */
void kmsan_task_create(struct task_struct *task)
{
	unsigned long irq_flags;

	irq_flags = kmsan_enter_runtime();
	kmsan_internal_task_create(task);
	kmsan_leave_runtime(irq_flags);
}
EXPORT_SYMBOL(kmsan_task_create);

/* Called from kernel/exit.c */
void kmsan_task_exit(struct task_struct *task)
{
	struct kmsan_context *ctx = &task->kmsan;

	if (!kmsan_ready || kmsan_in_runtime())
		return;

	ctx->allow_reporting = false;
}
EXPORT_SYMBOL(kmsan_task_exit);

/* Called from mm/slub.c */
void kmsan_slab_alloc(struct kmem_cache *s, void *object, gfp_t flags)
{
	unsigned long irq_flags;

	if (unlikely(object == NULL))
		return;
	if (!kmsan_ready || kmsan_in_runtime())
		return;
	/*
	 * There's a ctor or this is an RCU cache - do nothing. The memory
	 * status hasn't changed since last use.
	 */
	if (s->ctor || (s->flags & SLAB_TYPESAFE_BY_RCU))
		return;

	irq_flags = kmsan_enter_runtime();
	if (flags & __GFP_ZERO)
		kmsan_internal_unpoison_memory(object, s->object_size,
					       KMSAN_POISON_CHECK);
	else
		kmsan_internal_poison_memory(object, s->object_size, flags,
					     KMSAN_POISON_CHECK);
	kmsan_leave_runtime(irq_flags);
}
EXPORT_SYMBOL(kmsan_slab_alloc);

/* Called from mm/slub.c */
void kmsan_slab_free(struct kmem_cache *s, void *object)
{
	unsigned long irq_flags;

	if (!kmsan_ready || kmsan_in_runtime())
		return;

	/* RCU slabs could be legally used after free within the RCU period */
	if (unlikely(s->flags & (SLAB_TYPESAFE_BY_RCU | SLAB_POISON)))
		return;
	/*
	 * If there's a constructor, freed memory must remain in the same state
	 * till the next allocation. We cannot save its state to detect
	 * use-after-free bugs, instead we just keep it unpoisoned.
	 */
	if (s->ctor)
		return;
	irq_flags = kmsan_enter_runtime();
	kmsan_internal_poison_memory(object, s->object_size, GFP_KERNEL,
				     KMSAN_POISON_CHECK | KMSAN_POISON_FREE);
	kmsan_leave_runtime(irq_flags);
}
EXPORT_SYMBOL(kmsan_slab_free);

/* Called from mm/slub.c */
void kmsan_kmalloc_large(const void *ptr, size_t size, gfp_t flags)
{
	unsigned long irq_flags;

	if (unlikely(ptr == NULL))
		return;
	if (!kmsan_ready || kmsan_in_runtime())
		return;
	irq_flags = kmsan_enter_runtime();
	if (flags & __GFP_ZERO)
		kmsan_internal_unpoison_memory((void *)ptr, size,
					       /*checked*/ true);
	else
		kmsan_internal_poison_memory((void *)ptr, size, flags,
					     KMSAN_POISON_CHECK);
	kmsan_leave_runtime(irq_flags);
}
EXPORT_SYMBOL(kmsan_kmalloc_large);

/* Called from mm/slub.c */
void kmsan_kfree_large(const void *ptr)
{
	unsigned long irq_flags;
	struct page *page;

	if (!kmsan_ready || kmsan_in_runtime())
		return;
	irq_flags = kmsan_enter_runtime();
	page = virt_to_head_page((void *)ptr);
	BUG_ON(ptr != page_address(page));
	kmsan_internal_poison_memory((void *)ptr,
				     PAGE_SIZE << compound_order(page),
				     GFP_KERNEL,
				     KMSAN_POISON_CHECK | KMSAN_POISON_FREE);
	kmsan_leave_runtime(irq_flags);
}
EXPORT_SYMBOL(kmsan_kfree_large);

static unsigned long vmalloc_shadow(unsigned long addr)
{
	return (unsigned long)kmsan_get_metadata((void *)addr,
						 KMSAN_META_SHADOW);
}

static unsigned long vmalloc_origin(unsigned long addr)
{
	return (unsigned long)kmsan_get_metadata((void *)addr,
						 KMSAN_META_ORIGIN);
}

/* Called from mm/vmalloc.c */
void kmsan_vunmap_range_noflush(unsigned long start, unsigned long end)
{
	__vunmap_range_noflush(vmalloc_shadow(start), vmalloc_shadow(end));
	__vunmap_range_noflush(vmalloc_origin(start), vmalloc_origin(end));
	flush_cache_vmap(vmalloc_shadow(start), vmalloc_shadow(end));
	flush_cache_vmap(vmalloc_origin(start), vmalloc_origin(end));
}
EXPORT_SYMBOL(kmsan_vunmap_range_noflush);

/* Called from lib/ioremap.c */
/*
 * This function creates new shadow/origin pages for the physical pages mapped
 * into the virtual memory. If those physical pages already had shadow/origin,
 * those are ignored.
 */
void kmsan_ioremap_page_range(unsigned long start, unsigned long end,
			      phys_addr_t phys_addr, pgprot_t prot)
{
	gfp_t gfp_mask = GFP_KERNEL | __GFP_ZERO;
	struct page *shadow, *origin;
	unsigned long irq_flags;
	unsigned long off = 0;
	int i, nr;

	if (!kmsan_ready || kmsan_in_runtime())
		return;

	nr = (end - start) / PAGE_SIZE;
	irq_flags = kmsan_enter_runtime();
	for (i = 0; i < nr; i++, off += PAGE_SIZE) {
		shadow = alloc_pages(gfp_mask, 1);
		origin = alloc_pages(gfp_mask, 1);
		__vmap_pages_range_noflush(
			vmalloc_shadow(start + off),
			vmalloc_shadow(start + off + PAGE_SIZE), prot, &shadow,
			PAGE_SHIFT);
		__vmap_pages_range_noflush(
			vmalloc_origin(start + off),
			vmalloc_origin(start + off + PAGE_SIZE), prot, &origin,
			PAGE_SHIFT);
	}
	flush_cache_vmap(vmalloc_shadow(start), vmalloc_shadow(end));
	flush_cache_vmap(vmalloc_origin(start), vmalloc_origin(end));
	kmsan_leave_runtime(irq_flags);
}
EXPORT_SYMBOL(kmsan_ioremap_page_range);

void kmsan_iounmap_page_range(unsigned long start, unsigned long end)
{
	unsigned long v_shadow, v_origin;
	struct page *shadow, *origin;
	unsigned long irq_flags;
	int i, nr;

	if (!kmsan_ready || kmsan_in_runtime())
		return;

	nr = (end - start) / PAGE_SIZE;
	irq_flags = kmsan_enter_runtime();
	v_shadow = (unsigned long)vmalloc_shadow(start);
	v_origin = (unsigned long)vmalloc_origin(start);
	for (i = 0; i < nr; i++, v_shadow += PAGE_SIZE, v_origin += PAGE_SIZE) {
		shadow = kmsan_vmalloc_to_page_or_null((void *)v_shadow);
		origin = kmsan_vmalloc_to_page_or_null((void *)v_origin);
		__vunmap_range_noflush(v_shadow, vmalloc_shadow(end));
		__vunmap_range_noflush(v_origin, vmalloc_origin(end));
		if (shadow)
			__free_pages(shadow, 1);
		if (origin)
			__free_pages(origin, 1);
	}
	flush_cache_vmap(vmalloc_shadow(start), vmalloc_shadow(end));
	flush_cache_vmap(vmalloc_origin(start), vmalloc_origin(end));
	kmsan_leave_runtime(irq_flags);
}
EXPORT_SYMBOL(kmsan_iounmap_page_range);

/* Called from include/linux/uaccess.h, include/linux/uaccess.h */
void kmsan_copy_to_user(const void *to, const void *from, size_t to_copy,
			size_t left)
{
	if (!kmsan_ready || kmsan_in_runtime())
		return;
	/*
	 * At this point we've copied the memory already. It's hard to check it
	 * before copying, as the size of actually copied buffer is unknown.
	 */

	/* copy_to_user() may copy zero bytes. No need to check. */
	if (!to_copy)
		return;
	/* Or maybe copy_to_user() failed to copy anything. */
	if (to_copy <= left)
		return;
	if ((u64)to < TASK_SIZE) {
		/* This is a user memory access, check it. */
		kmsan_internal_check_memory((void *)from, to_copy - left, to,
					    REASON_COPY_TO_USER);
		return;
	}
	/* Otherwise this is a kernel memory access. This happens when a compat
	 * syscall passes an argument allocated on the kernel stack to a real
	 * syscall.
	 * Don't check anything, just copy the shadow of the copied bytes.
	 */
	kmsan_memmove_metadata((void *)to, (void *)from, to_copy - left);
}
EXPORT_SYMBOL(kmsan_copy_to_user);

/* Helper function to check an URB. */
void kmsan_handle_urb(const struct urb *urb, bool is_out)
{
	if (!urb)
		return;
	if (is_out)
		kmsan_internal_check_memory(urb->transfer_buffer,
					    urb->transfer_buffer_length,
					    /*user_addr*/ 0, REASON_SUBMIT_URB);
	else
		kmsan_internal_unpoison_memory(urb->transfer_buffer,
					       urb->transfer_buffer_length,
					       /*checked*/ false);
}
EXPORT_SYMBOL(kmsan_handle_urb);

static void kmsan_handle_dma_page(const void *addr, size_t size,
				  enum dma_data_direction dir)
{
	switch (dir) {
	case DMA_BIDIRECTIONAL:
		kmsan_internal_check_memory((void *)addr, size, /*user_addr*/ 0,
					    REASON_ANY);
		kmsan_internal_unpoison_memory((void *)addr, size,
					       /*checked*/ false);
		break;
	case DMA_TO_DEVICE:
		kmsan_internal_check_memory((void *)addr, size, /*user_addr*/ 0,
					    REASON_ANY);
		break;
	case DMA_FROM_DEVICE:
		kmsan_internal_unpoison_memory((void *)addr, size,
					       /*checked*/ false);
		break;
	case DMA_NONE:
		break;
	}
}

/* Helper function to handle DMA data transfers. */
void kmsan_handle_dma(struct page *page, size_t offset, size_t size,
		      enum dma_data_direction dir)
{
	u64 page_offset, to_go, addr;

	if (PageHighMem(page))
		return;
	addr = (u64)page_address(page) + offset;
	/*
	 * The kernel may occasionally give us adjacent DMA pages not belonging
	 * to the same allocation. Process them separately to avoid triggering
	 * internal KMSAN checks.
	 */
	while (size > 0) {
		page_offset = addr % PAGE_SIZE;
		to_go = min(PAGE_SIZE - page_offset, (u64)size);
		kmsan_handle_dma_page((void *)addr, to_go, dir);
		addr += to_go;
		size -= to_go;
	}
}
EXPORT_SYMBOL(kmsan_handle_dma);

void kmsan_handle_dma_sg(struct scatterlist *sg, int nents,
			 enum dma_data_direction dir)
{
	struct scatterlist *item;
	int i;

	for_each_sg (sg, item, nents, i)
		kmsan_handle_dma(sg_page(item), item->offset, item->length,
				 dir);
}
EXPORT_SYMBOL(kmsan_handle_dma_sg);

/* Functions from kmsan-checks.h follow. */
void kmsan_poison_memory(const void *address, size_t size, gfp_t flags)
{
	unsigned long irq_flags;

	if (!kmsan_ready || kmsan_in_runtime())
		return;
	irq_flags = kmsan_enter_runtime();
	/* The users may want to poison/unpoison random memory. */
	kmsan_internal_poison_memory((void *)address, size, flags,
				     KMSAN_POISON_NOCHECK);
	kmsan_leave_runtime(irq_flags);
}
EXPORT_SYMBOL(kmsan_poison_memory);

void kmsan_unpoison_memory(const void *address, size_t size)
{
	unsigned long irq_flags;

	if (!kmsan_ready || kmsan_in_runtime())
		return;

	irq_flags = kmsan_enter_runtime();
	/* The users may want to poison/unpoison random memory. */
	kmsan_internal_unpoison_memory((void *)address, size,
				       KMSAN_POISON_NOCHECK);
	kmsan_leave_runtime(irq_flags);
}
EXPORT_SYMBOL(kmsan_unpoison_memory);

void kmsan_gup_pgd_range(struct page **pages, int nr)
{
	void *page_addr;
	int i;

	/*
	 * gup_pgd_range() has just created a number of new pages that KMSAN
	 * treats as uninitialized. In the case they belong to the userspace
	 * memory, unpoison the corresponding kernel pages.
	 */
	for (i = 0; i < nr; i++) {
		if (PageHighMem(pages[i]))
			continue;
		page_addr = page_address(pages[i]);
		if (((u64)page_addr < TASK_SIZE) &&
		    ((u64)page_addr + PAGE_SIZE < TASK_SIZE))
			kmsan_unpoison_memory(page_addr, PAGE_SIZE);
	}
}
EXPORT_SYMBOL(kmsan_gup_pgd_range);

void kmsan_check_memory(const void *addr, size_t size)
{
	return kmsan_internal_check_memory((void *)addr, size, /*user_addr*/ 0,
					   REASON_ANY);
}
EXPORT_SYMBOL(kmsan_check_memory);

void kmsan_unpoison_pt_regs(struct pt_regs *regs)
{
	if (!kmsan_ready || !regs)
		return;
	kmsan_internal_unpoison_memory(regs, sizeof(*regs), /*checked*/ true);
}
EXPORT_SYMBOL(kmsan_unpoison_pt_regs);

void kmsan_instrumentation_begin(struct pt_regs *regs)
{
	struct kmsan_context_state *state = &kmsan_get_context()->cstate;

	if (state)
		__memset(state, 0, sizeof(struct kmsan_context_state));
	kmsan_unpoison_pt_regs(regs);
}
EXPORT_SYMBOL(kmsan_instrumentation_begin);
