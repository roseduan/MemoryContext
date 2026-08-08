/*-------------------------------------------------------------------------
 *
 * palloc.h
 *	  POSTGRES memory allocator definitions.
 *
 * This file contains the basic memory allocation interface that is
 * needed by almost every module in this library. Keep it lean!
 *
 * Memory allocation occurs within "contexts".  Every chunk obtained from
 * palloc()/MemoryContextAlloc() is allocated within a specific context.
 * The entire contents of a context can be freed easily and quickly by
 * resetting or deleting the context --- this is both faster and less
 * prone to memory-leakage bugs than releasing chunks individually.
 * We organize contexts into context trees to allow fine-grain control
 * over chunk lifetime while preserving the certainty that we will free
 * everything that should be freed.  See README for more info.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef PALLOC_H
#define PALLOC_H

#include "pg_compat.h"

/*
 * A memory context can have callback functions registered on it.  Any such
 * function will be called once just before the context is next reset or
 * deleted.  The MemoryContextCallback struct describing such a callback
 * typically would be allocated within the context itself, thereby avoiding
 * any need to manage it explicitly (the reset/delete action will free it).
 */
typedef void (*MemoryContextCallbackFunction) (void *arg);

struct MemoryContextCallback
{
	MemoryContextCallbackFunction func; /* function to call */
	void	   *arg;			/* argument to pass it */
	struct MemoryContextCallback *next; /* next in list of callbacks */
};

/*
 * CurrentMemoryContext is the default allocation context for palloc().
 * Avoid accessing it directly!  Instead, use MemoryContextSwitchTo()
 * to change the setting.
 */
extern PGDLLIMPORT MemoryContext CurrentMemoryContext;

/*
 * Flags for MemoryContextAllocExtended.
 */
#define MCXT_ALLOC_HUGE			0x01	/* allow huge allocation (> 1 GB) */
#define MCXT_ALLOC_NO_OOM		0x02	/* no failure if out-of-memory */
#define MCXT_ALLOC_ZERO			0x04	/* zero allocated memory */

/*
 * Fundamental memory-allocation operations (more are in memutils.h)
 */
extern void *MemoryContextAlloc(MemoryContext context, Size size);
extern void *MemoryContextAllocZero(MemoryContext context, Size size);
extern void *MemoryContextAllocExtended(MemoryContext context,
										Size size, int flags);
extern void *MemoryContextAllocAligned(MemoryContext context,
									   Size size, Size alignto, int flags);

extern void *palloc(Size size);
extern void *palloc0(Size size);
extern void *palloc_extended(Size size, int flags);
extern void *palloc_aligned(Size size, Size alignto, int flags);
pg_nodiscard extern void *repalloc(void *pointer, Size size);
pg_nodiscard extern void *repalloc_extended(void *pointer,
											Size size, int flags);
pg_nodiscard extern void *repalloc0(void *pointer, Size oldsize, Size size);
extern void pfree(void *pointer);

/*
 * Support for safe calculation of memory request sizes
 */
extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);
extern void *palloc_mul(Size s1, Size s2);
extern void *palloc0_mul(Size s1, Size s2);
extern void *palloc_mul_extended(Size s1, Size s2, int flags);
pg_nodiscard extern void *repalloc_mul(void *p, Size s1, Size s2);
pg_nodiscard extern void *repalloc_mul_extended(void *p, Size s1, Size s2,
												int flags);

/*
 * Variants with easier notation and more type safety
 */

/* Allocate space for one object of type "type" */
#define palloc_object(type) ((type *) palloc(sizeof(type)))
#define palloc0_object(type) ((type *) palloc0(sizeof(type)))

/* Allocate space for "count" objects of type "type" */
#define palloc_array(type, count) ((type *) palloc_mul(sizeof(type), count))
#define palloc0_array(type, count) ((type *) palloc0_mul(sizeof(type), count))
#define palloc_array_extended(type, count, flags) ((type *) palloc_mul_extended(sizeof(type), count, flags))

/*
 * Change size of allocation pointed to by "pointer" to have space for
 * "count" objects of type "type"
 */
#define repalloc_array(pointer, type, count) ((type *) repalloc_mul(pointer, sizeof(type), count))
#define repalloc0_array(pointer, type, oldcount, count) ((type *) repalloc0(pointer, mul_size(sizeof(type), oldcount), mul_size(sizeof(type), count)))
#define repalloc_array_extended(pointer, type, count, flags) ((type *) repalloc_mul_extended(pointer, sizeof(type), count, flags))

/* Higher-limit allocators. */
extern void *MemoryContextAllocHuge(MemoryContext context, Size size);
pg_nodiscard extern void *repalloc_huge(void *pointer, Size size);

static inline MemoryContext
MemoryContextSwitchTo(MemoryContext context)
{
	MemoryContext old = CurrentMemoryContext;

	CurrentMemoryContext = context;
	return old;
}

/* Registration of memory context reset/delete callbacks */
extern void MemoryContextRegisterResetCallback(MemoryContext context,
											   MemoryContextCallback *cb);
extern void MemoryContextUnregisterResetCallback(MemoryContext context,
												 MemoryContextCallback *cb);

/*
 * These are like standard strdup() except the copied string is
 * allocated in a context, not with malloc().
 */
extern char *MemoryContextStrdup(MemoryContext context, const char *string);
extern char *pstrdup(const char *in);
extern char *pnstrdup(const char *in, Size len);

extern char *pchomp(const char *in);

#endif							/* PALLOC_H */
