/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KMSAN API for subsystems.
 *
 * Copyright (C) 2017-2021 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */
#ifndef _LINUX_KMSAN_H
#define _LINUX_KMSAN_H

#include <linux/dma-direction.h>
#include <linux/gfp.h>
#include <linux/stackdepot.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

struct page;
struct kmem_cache;
struct task_struct;
struct scatterlist;
struct urb;

#ifdef CONFIG_KMSAN

/* These constants are defined in the MSan LLVM instrumentation pass. */
#define KMSAN_RETVAL_SIZE 800
#define KMSAN_PARAM_SIZE 800

struct kmsan_context_state {
	char param_tls[KMSAN_PARAM_SIZE];
	char retval_tls[KMSAN_RETVAL_SIZE];
	char va_arg_tls[KMSAN_PARAM_SIZE];
	char va_arg_origin_tls[KMSAN_PARAM_SIZE];
	u64 va_arg_overflow_size_tls;
	char param_origin_tls[KMSAN_PARAM_SIZE];
	depot_stack_handle_t retval_origin_tls;
};

#undef KMSAN_PARAM_SIZE
#undef KMSAN_RETVAL_SIZE

struct kmsan_context {
	struct kmsan_context_state cstate;
	int kmsan_in_runtime;
	bool allow_reporting;
};

/**
 * kmsan_initialize_shadow() - Initialize KMSAN shadow at boot time.
 *
 * Allocate and initialize KMSAN metadata for early allocations.
 */
void __init kmsan_initialize_shadow(void);

/**
 * kmsan_initialize() - Initialize KMSAN state and enable KMSAN.
 */
void __init kmsan_initialize(void);

/**
 * kmsan_memblock_free_pages() - handle freeing of memblock pages.
 *
 * Freed pages are either returned to buddy allocator or held back to be used
 * as metadata pages.
 */
bool __init kmsan_memblock_free_pages(struct page *page, unsigned int order);

/**
 * kmsan_task_create() - Initialize KMSAN state for the task.
 * @task: task to initialize.
 */
void kmsan_task_create(struct task_struct *task);

/**
 * kmsan_task_exit() - Notify KMSAN that a task has exited.
 * @task: task about to finish.
 */
void kmsan_task_exit(struct task_struct *task);

/**
 * kmsan_alloc_page() - Notify KMSAN about an alloc_pages() call.
 * @page:  struct page pointer returned by alloc_pages().
 * @order: order of allocated struct page.
 * @flags: GFP flags used by alloc_pages()
 *
 * KMSAN marks 1<<@order pages starting at @page as uninitialized, unless
 * @flags contain __GFP_ZERO.
 */
void kmsan_alloc_page(struct page *page, unsigned int order, gfp_t flags);

/**
 * kmsan_free_page() - Notify KMSAN about a free_pages() call.
 * @page:  struct page pointer passed to free_pages().
 * @order: order of deallocated struct page.
 *
 * KMSAN marks freed memory as uninitialized.
 */
void kmsan_free_page(struct page *page, unsigned int order);

/**
 * kmsan_copy_page_meta() - Copy KMSAN metadata between two pages.
 * @dst: destination page.
 * @src: source page.
 *
 * KMSAN copies the contents of metadata pages for @src into the metadata pages
 * for @dst. If @dst has no associated metadata pages, nothing happens.
 * If @src has no associated metadata pages, @dst metadata pages are unpoisoned.
 */
void kmsan_copy_page_meta(struct page *dst, struct page *src);

/**
 * kmsan_gup_pgd_range() - Notify KMSAN about a gup_pgd_range() call.
 * @pages: array of struct page pointers.
 * @nr:    array size.
 *
 * gup_pgd_range() creates new pages, some of which may belong to the userspace
 * memory. In that case KMSAN marks them as initialized.
 */
void kmsan_gup_pgd_range(struct page **pages, int nr);

/**
 * kmsan_slab_alloc() - Notify KMSAN about a slab allocation.
 * @s:      slab cache the object belongs to.
 * @object: object pointer.
 * @flags:  GFP flags passed to the allocator.
 *
 * Depending on cache flags and GFP flags, KMSAN sets up the metadata of the
 * newly created object, marking it as initialized or uninitialized.
 */
void kmsan_slab_alloc(struct kmem_cache *s, void *object, gfp_t flags);

/**
 * kmsan_slab_free() - Notify KMSAN about a slab deallocation.
 * @s:      slab cache the object belongs to.
 * @object: object pointer.
 *
 * KMSAN marks the freed object as uninitialized.
 */
void kmsan_slab_free(struct kmem_cache *s, void *object);

/**
 * kmsan_kmalloc_large() - Notify KMSAN about a large slab allocation.
 * @ptr:   object pointer.
 * @size:  object size.
 * @flags: GFP flags passed to the allocator.
 *
 * Similar to kmsan_slab_alloc(), but for large allocations.
 */
void kmsan_kmalloc_large(const void *ptr, size_t size, gfp_t flags);

/**
 * kmsan_kfree_large() - Notify KMSAN about a large slab deallocation.
 * @ptr: object pointer.
 *
 * Similar to kmsan_slab_free(), but for large allocations.
 */
void kmsan_kfree_large(const void *ptr);

/**
 * kmsan_map_kernel_range_noflush() - Notify KMSAN about a vmap.
 * @start: start address of vmapped range.
 * @size:  size of vmapped range.
 * @prot:  page protection flags used for vmap.
 * @pages: array of pages.
 *
 * KMSAN maps shadow and origin pages of @pages into contiguous ranges in
 * vmalloc metadata address range.
 */
void kmsan_vmap_pages_range_noflush(unsigned long start, unsigned long end,
				    pgprot_t prot, struct page **pages,
				    unsigned int page_shift);

/**
 * kmsan_vunmap_kernel_range_noflush() - Notify KMSAN about a vunmap.
 * @addr: start address of vunmapped range.
 * @size:  end address of vunmapped range.
 *
 * KMSAN unmaps the contiguous metadata ranges created by
 * kmsan_map_kernel_range_noflush().
 */
void kmsan_vunmap_range_noflush(unsigned long start, unsigned long end);

/**
 * kmsan_ioremap_page_range() - Notify KMSAN about a ioremap_page_range() call.
 * @addr:      range start.
 * @end:       range end.
 * @phys_addr: physical range start.
 * @prot:      page protection flags used for ioremap_page_range().
 *
 * KMSAN creates new metadata pages for the physical pages mapped into the
 * virtual memory.
 */
void kmsan_ioremap_page_range(unsigned long addr, unsigned long end,
			      phys_addr_t phys_addr, pgprot_t prot);

/**
 * kmsan_iounmap_page_range() - Notify KMSAN about a iounmap_page_range() call.
 * @start: range start.
 * @end:   range end.
 *
 * KMSAN unmaps the metadata pages for the given range and, unlike for
 * vunmap_page_range(), also deallocates them.
 */
void kmsan_iounmap_page_range(unsigned long start, unsigned long end);

/**
 * kmsan_handle_dma() - Handle a DMA data transfer.
 * @page:   first page of the buffer.
 * @offset: offset of the buffer within the first page.
 * @size:   buffer size.
 * @dir:    one of possible dma_data_direction values.
 *
 * Depending on @direction, KMSAN:
 * * checks the buffer, if it is copied to device;
 * * initializes the buffer, if it is copied from device;
 * * does both, if this is a DMA_BIDIRECTIONAL transfer.
 */
void kmsan_handle_dma(struct page *page, size_t offset, size_t size,
		      enum dma_data_direction dir);

/**
 * kmsan_handle_dma_sg() - Handle a DMA transfer using scatterlist.
 * @sg:    scatterlist holding DMA buffers.
 * @nents: number of scatterlist entries.
 * @dir:   one of possible dma_data_direction values.
 *
 * Depending on @direction, KMSAN:
 * * checks the buffers in the scatterlist, if they are copied to device;
 * * initializes the buffers, if they are copied from device;
 * * does both, if this is a DMA_BIDIRECTIONAL transfer.
 */
void kmsan_handle_dma_sg(struct scatterlist *sg, int nents,
			 enum dma_data_direction dir);

/**
 * kmsan_handle_urb() - Handle a USB data transfer.
 * @urb:    struct urb pointer.
 * @is_out: data transfer direction (true means output to hardware)
 *
 * If @is_out is true, KMSAN checks the transfer buffer of @urb. Otherwise,
 * KMSAN initializes the transfer buffer.
 */
void kmsan_handle_urb(const struct urb *urb, bool is_out);

/**
 * kmsan_unpoison_pt_regs() - helper that marks struct pt_regs as initialized.
 * @regs:	pointer to struct pt_regs.
 */
void kmsan_unpoison_pt_regs(struct pt_regs *regs);

void kmsan_instrumentation_begin(struct pt_regs *regs);

#else

static inline void kmsan_initialize_shadow(void)
{
}
static inline void kmsan_initialize(void)
{
}
static inline bool kmsan_memblock_free_pages(struct page *page,
					     unsigned int order)
{
	return true;
}

static inline void kmsan_task_create(struct task_struct *task)
{
}
static inline void kmsan_task_exit(struct task_struct *task)
{
}

static inline int kmsan_alloc_page(struct page *page, unsigned int order,
				   gfp_t flags)
{
	return 0;
}
static inline void kmsan_free_page(struct page *page, unsigned int order)
{
}
static inline void kmsan_copy_page_meta(struct page *dst, struct page *src)
{
}
static inline void kmsan_gup_pgd_range(struct page **pages, int nr)
{
}

static inline void kmsan_slab_alloc(struct kmem_cache *s, void *object,
				    gfp_t flags)
{
}
static inline void kmsan_slab_free(struct kmem_cache *s, void *object)
{
}
static inline void kmsan_kmalloc_large(const void *ptr, size_t size,
				       gfp_t flags)
{
}
static inline void kmsan_kfree_large(const void *ptr)
{
}
static inline void kmsan_map_kernel_range_noflush(unsigned long start,
						  unsigned long size,
						  pgprot_t prot,
						  struct page **pages)
{
}
static inline void kmsan_unmap_kernel_range(unsigned long start,
					    unsigned long size)
{
}

static inline void kmsan_ioremap_page_range(unsigned long start,
					    unsigned long end,
					    phys_addr_t phys_addr,
					    pgprot_t prot)
{
}
static inline void kmsan_iounmap_page_range(unsigned long start,
					    unsigned long end)
{
}

static inline void kmsan_handle_dma(struct page *page, size_t offset,
				    size_t size, enum dma_data_direction dir)
{
}
static inline void kmsan_handle_dma_sg(struct scatterlist *sg, int nents,
				       enum dma_data_direction dir)
{
}
static inline void kmsan_handle_urb(const struct urb *urb, bool is_out)
{
}
static inline void kmsan_unpoison_pt_regs(struct pt_regs *regs)
{
}

void kmsan_instrumentation_begin(struct pt_regs *regs)
{
}

#endif

#endif /* _LINUX_KMSAN_H */
