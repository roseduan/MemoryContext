#ifndef MCTX_INTERNAL_H
#define MCTX_INTERNAL_H

#include <stdlib.h>

#include "memutils.h"
#include "palloc.h"
#include "pg_compat.h"

/*-------------------------------------------------------------------------
 *
 * mctx_internal.h
 *	  This file contains declarations for memory allocation utility
 *	  functions for internal use.
 *
 *-------------------------------------------------------------------------
 */

/* These functions implement the MemoryContext API for AllocSet context. */
extern void *AllocSetAlloc(MemoryContext context, Size size, int flags);
extern void AllocSetFree(void *pointer);
extern void *AllocSetRealloc(void *pointer, Size size, int flags);
extern void AllocSetReset(MemoryContext context);
extern void AllocSetDelete(MemoryContext context);
extern MemoryContext AllocSetGetChunkContext(void *pointer);
extern Size AllocSetGetChunkSpace(void *pointer);
extern bool AllocSetIsEmpty(MemoryContext context);
extern void AllocSetStats(MemoryContext context,
						  MemoryStatsPrintFunc printfunc, void *passthru,
						  MemoryContextCounters *totals,
						  bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
extern void AllocSetCheck(MemoryContext context);
#endif

/*
 * These functions support the implementation of palloc_aligned() and are not
 * part of a fully-fledged MemoryContext type.
 */
extern void AlignedAllocFree(void *pointer);
extern void *AlignedAllocRealloc(void *pointer, Size size, int flags);
extern MemoryContext AlignedAllocGetChunkContext(void *pointer);
extern Size AlignedAllocGetChunkSpace(void *pointer);

/*
 * How many extra bytes do we need to request in order to ensure that we can
 * align a pointer to 'alignto'.  Since palloc'd pointers are already aligned
 * to MAXIMUM_ALIGNOF we can subtract that amount.  We also need to make sure
 * there is enough space for the redirection MemoryChunk.
 */
#define PallocAlignedExtraBytes(alignto) \
	((alignto) + (sizeof(MemoryChunk) - MAXIMUM_ALIGNOF))

/*
 * MemoryContextMethodID
 *		A unique identifier for each MemoryContext implementation which
 *		indicates the index into the mcxt_methods[] array. See mcxt.c.
 *
 * For robust error detection, ensure that MemoryContextMethodID has a value
 * for each possible bit-pattern of MEMORY_CONTEXT_METHODID_MASK, and make
 * dummy entries for unused IDs in the mcxt_methods[] array.  We also try
 * to avoid using bit-patterns as valid IDs if they are likely to occur in
 * garbage data, or if they could falsely match on chunks that are really from
 * malloc not palloc.  (We can't tell that for most malloc implementations,
 * but it happens that glibc stores flag bits in the same place where we put
 * the MemoryContextMethodID, so the possible values are predictable for it.)
 */
typedef enum MemoryContextMethodID
{
	MCTX_0_RESERVED_UNUSEDMEM_ID,	/* 0000 occurs in never-used memory */
	MCTX_1_RESERVED_GLIBC_ID,	/* glibc malloc'd chunks usually match 0001 */
	MCTX_2_RESERVED_GLIBC_ID,	/* glibc malloc'd chunks > 128kB match 0010 */
	MCTX_ASET_ID,
	MCTX_GENERATION_ID,
	MCTX_SLAB_ID,
	MCTX_ALIGNED_REDIRECT_ID,
	MCTX_BUMP_ID,
	MCTX_8_UNUSED_ID,
	MCTX_9_UNUSED_ID,
	MCTX_10_UNUSED_ID,
	MCTX_11_UNUSED_ID,
	MCTX_12_UNUSED_ID,
	MCTX_13_UNUSED_ID,
	MCTX_14_UNUSED_ID,
	MCTX_15_RESERVED_WIPEDMEM_ID	/* 1111 occurs in wipe_mem'd memory */
} MemoryContextMethodID;

#define MEMORY_CONTEXT_METHODID_BITS 4
#define MEMORY_CONTEXT_METHODID_MASK \
	((((uint64) 1) << MEMORY_CONTEXT_METHODID_BITS) - 1)

/*
 * This routine handles the context-type-independent part of memory
 * context creation.  It's intended to be called from context-type-
 * specific creation routines, and noplace else.
 */
extern void MemoryContextCreate(MemoryContext node,
								NodeTag tag,
								MemoryContextMethodID method_id,
								MemoryContext parent,
								const char *name);

/*
 * MemoryContextAllocationFailure
 *		For use by MemoryContextMethods implementations to handle when malloc
 *		returns NULL.  The behavior is specific to whether MCXT_ALLOC_NO_OOM
 *		is in 'flags': if it is, NULL is returned; otherwise this reports the
 *		failure and aborts, and does not return control to the caller.
 *
 * MemoryContextSizeFailure
 *		For use by MemoryContextMethods implementations to handle invalid
 *		memory allocation request sizes.  Always aborts.
 */
extern void *MemoryContextAllocationFailure(MemoryContext context, Size size,
											int flags);
pg_noreturn extern void MemoryContextSizeFailure(MemoryContext context,
												 Size size, int flags);

static inline void
MemoryContextCheckSize(MemoryContext context, Size size, int flags)
{
	if (unlikely(!AllocSizeIsValid(size)))
	{
		if (!(flags & MCXT_ALLOC_HUGE) || !AllocHugeSizeIsValid(size))
			MemoryContextSizeFailure(context, size, flags);
	}
}

/*
 * stderr diagnostics paired with the abort() semantics described above.
 * mctx_report_oom is only ever called on the non-NO_OOM path; the NO_OOM
 * path skips the report entirely and returns NULL silently instead.
 */
pg_noreturn static inline void
mctx_report_oom(Size size, const char *ctxname)
{
	fprintf(stderr,
			"out of memory: failed on request of size %zu in memory context \"%s\"\n",
			size, ctxname ? ctxname : "?");
	abort();
}

pg_noreturn static inline void
mctx_report_bad_size(Size size)
{
	fprintf(stderr, "invalid memory alloc request size %zu\n", size);
	abort();
}

/*
 * Guards internal bookkeeping invariants (e.g. a chunk's block header
 * doesn't point where it should, or a foreign/garbage pointer was passed
 * to pfree/repalloc) -- corruption, not OOM. Continuing with a corrupt
 * heap is worse than stopping, so this always aborts.
 */
pg_noreturn static inline void
mctx_report_corruption(const char *msg, void *ptr)
{
	fprintf(stderr, "memory context corruption: %s %p\n", msg, ptr);
	abort();
}

/*
 * add_size()/mul_size() overflow: there's no NULL-like sentinel for a Size
 * return value, and silently truncating a size computation would corrupt
 * whatever allocation it feeds into -- so this is the same "unsafe to
 * continue" case as corruption, not a normal recoverable failure.
 */
pg_noreturn static inline void
mctx_report_size_overflow(void)
{
	fprintf(stderr, "requested size overflows size_t\n");
	abort();
}

#endif							/* MCTX_INTERNAL_H */
