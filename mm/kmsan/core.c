// SPDX-License-Identifier: GPL-2.0
/*
 * KMSAN runtime library.
 *
 * Copyright (C) 2017-2021 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */

#include <asm/page.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kmsan.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmzone.h>
#include <linux/percpu-defs.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include "../slab.h"
#include "kmsan.h"

#define MAX_CHAIN_DEPTH 7

bool kmsan_ready;
/*
 * According to Documentation/x86/kernel-stacks, kernel code can run on the
 * following stacks:
 * - regular task stack - when executing the task code
 *  - interrupt stack - when handling external hardware interrupts and softirqs
 *  - NMI stack
 * 0 is for regular interrupts, 1 for softirqs, 2 for NMI.
 * Because interrupts may nest, trying to use a new context for every new
 * interrupt.
 */
DEFINE_PER_CPU(struct kmsan_context, kmsan_percpu_ctx);

void kmsan_internal_task_create(struct task_struct *task)
{
	struct kmsan_context *ctx = &task->kmsan;

	__memset(ctx, 0, sizeof(struct kmsan_context));
	ctx->allow_reporting = true;
}

void kmsan_internal_poison_memory(void *address, size_t size, gfp_t flags,
				  unsigned int poison_flags)
{
	u32 extra_bits =
		kmsan_extra_bits(/*depth*/ 0, poison_flags & KMSAN_POISON_FREE);
	bool checked = poison_flags & KMSAN_POISON_CHECK;
	depot_stack_handle_t handle;

	handle = kmsan_save_stack_with_flags(flags, extra_bits);
	kmsan_internal_set_shadow_origin(address, size, -1, handle, checked);
}

void kmsan_internal_unpoison_memory(void *address, size_t size, bool checked)
{
	kmsan_internal_set_shadow_origin(address, size, 0, 0, checked);
}

depot_stack_handle_t kmsan_save_stack_with_flags(gfp_t flags,
						 unsigned int reserved)
{
	unsigned long entries[KMSAN_STACK_DEPTH];
	unsigned int nr_entries;

	nr_entries = stack_trace_save(entries, KMSAN_STACK_DEPTH, 0);
	nr_entries = filter_irq_stacks(entries, nr_entries);

	/* Don't sleep (see might_sleep_if() in __alloc_pages_nodemask()). */
	flags &= ~__GFP_DIRECT_RECLAIM;

	return stack_depot_save_extra(entries, nr_entries, reserved, flags);
}

/* Copy the metadata following the memmove() behavior. */
void kmsan_memmove_metadata(void *dst, void *src, size_t n)
{
	depot_stack_handle_t old_origin = 0, chain_origin, new_origin = 0;
	int src_slots, dst_slots, i, iter, step, skip_bits;
	depot_stack_handle_t *origin_src, *origin_dst;
	void *shadow_src, *shadow_dst;
	u32 *align_shadow_src, shadow;
	bool backwards;

	shadow_dst = kmsan_get_metadata(dst, KMSAN_META_SHADOW);
	if (!shadow_dst)
		return;
	BUG_ON(!kmsan_metadata_is_contiguous(dst, n));

	shadow_src = kmsan_get_metadata(src, KMSAN_META_SHADOW);
	if (!shadow_src) {
		/*
		 * |src| is untracked: zero out destination shadow, ignore the
		 * origins, we're done.
		 */
		__memset(shadow_dst, 0, n);
		return;
	}
	BUG_ON(!kmsan_metadata_is_contiguous(src, n));

	__memmove(shadow_dst, shadow_src, n);

	origin_dst = kmsan_get_metadata(dst, KMSAN_META_ORIGIN);
	origin_src = kmsan_get_metadata(src, KMSAN_META_ORIGIN);
	BUG_ON(!origin_dst || !origin_src);
	src_slots = (ALIGN((u64)src + n, KMSAN_ORIGIN_SIZE) -
		     ALIGN_DOWN((u64)src, KMSAN_ORIGIN_SIZE)) /
		    KMSAN_ORIGIN_SIZE;
	dst_slots = (ALIGN((u64)dst + n, KMSAN_ORIGIN_SIZE) -
		     ALIGN_DOWN((u64)dst, KMSAN_ORIGIN_SIZE)) /
		    KMSAN_ORIGIN_SIZE;
	BUG_ON(!src_slots || !dst_slots);
	BUG_ON((src_slots < 1) || (dst_slots < 1));
	BUG_ON((src_slots - dst_slots > 1) || (dst_slots - src_slots < -1));

	backwards = dst > src;
	i = backwards ? min(src_slots, dst_slots) - 1 : 0;
	iter = backwards ? -1 : 1;

	align_shadow_src =
		(u32 *)ALIGN_DOWN((u64)shadow_src, KMSAN_ORIGIN_SIZE);
	for (step = 0; step < min(src_slots, dst_slots); step++, i += iter) {
		BUG_ON(i < 0);
		shadow = align_shadow_src[i];
		if (i == 0) {
			/*
			 * If |src| isn't aligned on KMSAN_ORIGIN_SIZE, don't
			 * look at the first |src % KMSAN_ORIGIN_SIZE| bytes
			 * of the first shadow slot.
			 */
			skip_bits = ((u64)src % KMSAN_ORIGIN_SIZE) * 8;
			shadow = (shadow << skip_bits) >> skip_bits;
		}
		if (i == src_slots - 1) {
			/*
			 * If |src + n| isn't aligned on
			 * KMSAN_ORIGIN_SIZE, don't look at the last
			 * |(src + n) % KMSAN_ORIGIN_SIZE| bytes of the
			 * last shadow slot.
			 */
			skip_bits = (((u64)src + n) % KMSAN_ORIGIN_SIZE) * 8;
			shadow = (shadow >> skip_bits) << skip_bits;
		}
		/*
		 * Overwrite the origin only if the corresponding
		 * shadow is nonempty.
		 */
		if (origin_src[i] && (origin_src[i] != old_origin) && shadow) {
			old_origin = origin_src[i];
			chain_origin = kmsan_internal_chain_origin(old_origin);
			/*
			 * kmsan_internal_chain_origin() may return
			 * NULL, but we don't want to lose the previous
			 * origin value.
			 */
			if (chain_origin)
				new_origin = chain_origin;
			else
				new_origin = old_origin;
		}
		if (shadow)
			origin_dst[i] = new_origin;
		else
			origin_dst[i] = 0;
	}
}

depot_stack_handle_t kmsan_internal_chain_origin(depot_stack_handle_t id)
{
	unsigned long entries[3];
	u32 extra_bits;
	int depth;
	bool uaf;

	if (!id)
		return id;
	/*
	 * Make sure we have enough spare bits in |id| to hold the UAF bit and
	 * the chain depth.
	 */
	BUILD_BUG_ON((1 << STACK_DEPOT_EXTRA_BITS) <= (MAX_CHAIN_DEPTH << 1));

	extra_bits = stack_depot_get_extra_bits(id);
	depth = kmsan_depth_from_eb(extra_bits);
	uaf = kmsan_uaf_from_eb(extra_bits);

	if (depth >= MAX_CHAIN_DEPTH) {
		static atomic_long_t kmsan_skipped_origins;
		long skipped = atomic_long_inc_return(&kmsan_skipped_origins);

		if (skipped % 10000 == 0) {
			pr_warn("not chained %d origins\n", skipped);
			dump_stack();
			kmsan_print_origin(id);
		}
		return id;
	}
	depth++;
	extra_bits = kmsan_extra_bits(depth, uaf);

	entries[0] = KMSAN_CHAIN_MAGIC_ORIGIN;
	entries[1] = kmsan_save_stack_with_flags(GFP_ATOMIC, extra_bits);
	entries[2] = id;
	return stack_depot_save_extra(entries, ARRAY_SIZE(entries), extra_bits,
				      GFP_ATOMIC);
}

void kmsan_internal_set_shadow_origin(void *addr, size_t size, int b,
				      u32 origin, bool checked)
{
	u64 address = (u64)addr;
	void *shadow_start;
	u32 *origin_start;
	size_t pad = 0;
	int i;

	BUG_ON(!kmsan_metadata_is_contiguous(addr, size));
	shadow_start = kmsan_get_metadata(addr, KMSAN_META_SHADOW);
	if (!shadow_start) {
		/*
		 * kmsan_metadata_is_contiguous() is true, so either all shadow
		 * and origin pages are NULL, or all are non-NULL.
		 */
		if (checked) {
			pr_err("%s: not memsetting %d bytes starting at %px, because the shadow is NULL\n",
			       __func__, size, addr);
			BUG();
		}
		return;
	} else {
		__memset(shadow_start, b, size);
	}

	if (!IS_ALIGNED(address, KMSAN_ORIGIN_SIZE)) {
		pad = address % KMSAN_ORIGIN_SIZE;
		address -= pad;
		size += pad;
	}
	size = ALIGN(size, KMSAN_ORIGIN_SIZE);
	origin_start =
		(u32 *)kmsan_get_metadata((void *)address, KMSAN_META_ORIGIN);

	/* Shadow is non-NULL here, so origin must also be valid. */
	BUG_ON(!origin_start);
	for (i = 0; i < size / KMSAN_ORIGIN_SIZE; i++)
		origin_start[i] = origin;
}

struct page *kmsan_vmalloc_to_page_or_null(void *vaddr)
{
	struct page *page;

	if (!kmsan_internal_is_vmalloc_addr(vaddr) &&
	    !kmsan_internal_is_module_addr(vaddr))
		return NULL;
	page = vmalloc_to_page(vaddr);
	if (pfn_valid(page_to_pfn(page)))
		return page;
	else
		return NULL;
}

void kmsan_internal_check_memory(void *addr, size_t size, const void *user_addr,
				 int reason)
{
	depot_stack_handle_t cur_origin = 0, new_origin = 0;
	unsigned long addr64 = (unsigned long)addr;
	depot_stack_handle_t *origin = NULL;
	unsigned char *shadow = NULL;
	unsigned long irq_flags;
	int cur_off_start = -1;
	int i, chunk_size;
	size_t pos = 0;

	if (!size)
		return;
	BUG_ON(!kmsan_metadata_is_contiguous(addr, size));
	while (pos < size) {
		chunk_size = min(size - pos,
				 PAGE_SIZE - ((addr64 + pos) % PAGE_SIZE));
		shadow = kmsan_get_metadata((void *)(addr64 + pos),
					    KMSAN_META_SHADOW);
		if (!shadow) {
			/*
			 * This page is untracked. If there were uninitialized
			 * bytes before, report them.
			 */
			if (cur_origin) {
				irq_flags = kmsan_enter_runtime();
				kmsan_report(cur_origin, addr, size,
					     cur_off_start, pos - 1, user_addr,
					     reason);
				kmsan_leave_runtime(irq_flags);
			}
			cur_origin = 0;
			cur_off_start = -1;
			pos += chunk_size;
			continue;
		}
		for (i = 0; i < chunk_size; i++) {
			if (!shadow[i]) {
				/*
				 * This byte is unpoisoned. If there were
				 * poisoned bytes before, report them.
				 */
				if (cur_origin) {
					irq_flags = kmsan_enter_runtime();
					kmsan_report(cur_origin, addr, size,
						     cur_off_start, pos + i - 1,
						     user_addr, reason);
					kmsan_leave_runtime(irq_flags);
				}
				cur_origin = 0;
				cur_off_start = -1;
				continue;
			}
			origin = kmsan_get_metadata((void *)(addr64 + pos + i),
						    KMSAN_META_ORIGIN);
			BUG_ON(!origin);
			new_origin = *origin;
			/*
			 * Encountered new origin - report the previous
			 * uninitialized range.
			 */
			if (cur_origin != new_origin) {
				if (cur_origin) {
					irq_flags = kmsan_enter_runtime();
					kmsan_report(cur_origin, addr, size,
						     cur_off_start, pos + i - 1,
						     user_addr, reason);
					kmsan_leave_runtime(irq_flags);
				}
				cur_origin = new_origin;
				cur_off_start = pos + i;
			}
		}
		pos += chunk_size;
	}
	BUG_ON(pos != size);
	if (cur_origin) {
		irq_flags = kmsan_enter_runtime();
		kmsan_report(cur_origin, addr, size, cur_off_start, pos - 1,
			     user_addr, reason);
		kmsan_leave_runtime(irq_flags);
	}
}

bool kmsan_metadata_is_contiguous(void *addr, size_t size)
{
	char *cur_shadow = NULL, *next_shadow = NULL, *cur_origin = NULL,
	     *next_origin = NULL;
	u64 cur_addr = (u64)addr, next_addr = cur_addr + PAGE_SIZE;
	depot_stack_handle_t *origin_p;
	bool all_untracked = false;

	if (!size)
		return true;

	/* The whole range belongs to the same page. */
	if (ALIGN_DOWN(cur_addr + size - 1, PAGE_SIZE) ==
	    ALIGN_DOWN(cur_addr, PAGE_SIZE))
		return true;

	cur_shadow = kmsan_get_metadata((void *)cur_addr, /*is_origin*/ false);
	if (!cur_shadow)
		all_untracked = true;
	cur_origin = kmsan_get_metadata((void *)cur_addr, /*is_origin*/ true);
	if (all_untracked && cur_origin)
		goto report;

	for (; next_addr < (u64)addr + size;
	     cur_addr = next_addr, cur_shadow = next_shadow,
	     cur_origin = next_origin, next_addr += PAGE_SIZE) {
		next_shadow = kmsan_get_metadata((void *)next_addr, false);
		next_origin = kmsan_get_metadata((void *)next_addr, true);
		if (all_untracked) {
			if (next_shadow || next_origin)
				goto report;
			if (!next_shadow && !next_origin)
				continue;
		}
		if (((u64)cur_shadow == ((u64)next_shadow - PAGE_SIZE)) &&
		    ((u64)cur_origin == ((u64)next_origin - PAGE_SIZE)))
			continue;
		goto report;
	}
	return true;

report:
	pr_err("%s: attempting to access two shadow page ranges.\n", __func__);
	pr_err("Access of size %d at %px.\n", size, addr);
	pr_err("Addresses belonging to different ranges: %px and %px\n",
	       cur_addr, next_addr);
	pr_err("page[0].shadow: %px, page[1].shadow: %px\n", cur_shadow,
	       next_shadow);
	pr_err("page[0].origin: %px, page[1].origin: %px\n", cur_origin,
	       next_origin);
	origin_p = kmsan_get_metadata(addr, KMSAN_META_ORIGIN);
	if (origin_p) {
		pr_err("Origin: %08x\n", *origin_p);
		kmsan_print_origin(*origin_p);
	} else {
		pr_err("Origin: unavailable\n");
	}
	return false;
}

bool kmsan_internal_is_module_addr(void *vaddr)
{
	return ((u64)vaddr >= MODULES_VADDR) && ((u64)vaddr < MODULES_END);
}

bool kmsan_internal_is_vmalloc_addr(void *addr)
{
	return ((u64)addr >= VMALLOC_START) && ((u64)addr < VMALLOC_END);
}
