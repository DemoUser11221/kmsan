/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_COMPILER_TYPES_H
#error "Please don't include <linux/compiler-clang.h> directly, include <linux/compiler.h> instead."
#endif

/* Compiler specific definitions for Clang compiler */

/* same as gcc, this was present in clang-2.6 so we can assume it works
 * with any version that can compile the kernel
 */
#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)

/* all clang versions usable with the kernel support KASAN ABI version 5 */
#define KASAN_ABI_VERSION 5

/*
 * Note: Checking __has_feature(*_sanitizer) is only true if the feature is
 * enabled. Therefore it is not required to additionally check defined(CONFIG_*)
 * to avoid adding redundant attributes in other configurations.
 */

#if __has_feature(address_sanitizer) || __has_feature(hwaddress_sanitizer)
/* Emulate GCC's __SANITIZE_ADDRESS__ flag */
#define __SANITIZE_ADDRESS__
#define __no_sanitize_address \
		__attribute__((no_sanitize("address", "hwaddress")))
#else
#define __no_sanitize_address
#endif

#if __has_feature(thread_sanitizer)
/* emulate gcc's __SANITIZE_THREAD__ flag */
#define __SANITIZE_THREAD__
#define __no_sanitize_thread \
		__attribute__((no_sanitize("thread")))
#else
#define __no_sanitize_thread
#endif

#if defined(CONFIG_ARCH_USE_BUILTIN_BSWAP)
#define __HAVE_BUILTIN_BSWAP32__
#define __HAVE_BUILTIN_BSWAP64__
#define __HAVE_BUILTIN_BSWAP16__
#endif /* CONFIG_ARCH_USE_BUILTIN_BSWAP */

#if __has_feature(undefined_behavior_sanitizer)
/* GCC does not have __SANITIZE_UNDEFINED__ */
#define __no_sanitize_undefined \
		__attribute__((no_sanitize("undefined")))
#else
#define __no_sanitize_undefined
#endif

#if __has_feature(memory_sanitizer)
#define __SANITIZE_MEMORY__
/*
 * Unlike other sanitizers, KMSAN still inserts code into functions marked with
 * no_sanitize("kernel-memory"). Using disable_sanitizer_instrumentation
 * provides the behavior consistent with other __no_sanitize_ attributes,
 * guaranteeing that __no_sanitize_memory functions remain uninstrumented.
 */
#define __no_sanitize_memory __attribute__((disable_sanitizer_instrumentation))

/*
 * The __no_kmsan_checks attribute ensures that a function does not produce
 * false positive reports by:
 *  - initializing all local variables and memory stores in this function;
 *  - skipping all shadow checks;
 *  - passing initialized arguments to this function's callees.
 */
#define __no_kmsan_checks __attribute__((no_sanitize("kernel-memory")))
#else
#define __no_sanitize_memory
#define __no_kmsan_checks
#endif

/*
 * Support for __has_feature(coverage_sanitizer) was added in Clang 13 together
 * with no_sanitize("coverage"). Prior versions of Clang support coverage
 * instrumentation, but cannot be queried for support by the preprocessor.
 */
#if __has_feature(coverage_sanitizer)
#define __no_sanitize_coverage __attribute__((no_sanitize("coverage")))
#else
#define __no_sanitize_coverage
#endif

#if __has_feature(shadow_call_stack)
# define __noscs	__attribute__((__no_sanitize__("shadow-call-stack")))
#endif

#define __nocfi		__attribute__((__no_sanitize__("cfi")))
#define __cficanonical	__attribute__((__cfi_canonical_jump_table__))
