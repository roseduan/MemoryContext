#ifndef PG_COMPAT_H
#define PG_COMPAT_H

/*
 * Minimal definitions standing in for the slice of PostgreSQL's
 * c.h/postgres.h that mcxt.c and aset.c need: configure-detected feature
 * macros, alignment and overflow-checked arithmetic helpers, and a small
 * NodeTag enum covering only the memory-context implementations this
 * library has.
 *
 * Assumes a gcc/clang-compatible compiler on a 64-bit target.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>

typedef size_t Size;
typedef uint32_t uint32;
typedef uint64_t uint64;

/* gcc/clang provide these unconditionally */
#define HAVE__BUILTIN_CLZ
#define HAVE_BITSCAN_REVERSE
#define HAVE__BUILTIN_CONSTANT_P

/* No DLL export/import distinction outside Windows. */
#define PGDLLIMPORT

#define Assert(p) assert(p)
#define AssertMacro(p) ((void) assert(p))
#define PG_USED_FOR_ASSERTS_ONLY

/* Allocation is never disallowed on account of being inside a critical
 * section here -- there's no signal handler re-entering this code. */
#define AssertNotInCriticalSection(context) ((void) 0)

#define pg_nodiscard __attribute__((warn_unused_result))
#define pg_noinline __attribute__((noinline))
#define pg_noreturn _Noreturn

#if defined(__GNUC__) || defined(__clang__)
#define likely(x)	__builtin_expect((x) != 0, 1)
#define unlikely(x) __builtin_expect((x) != 0, 0)
#else
#define likely(x)	((x) != 0)
#define unlikely(x) ((x) != 0)
#endif

#define StaticAssertStmt(condition, errmessage) \
	do { _Static_assert(condition, errmessage); } while (0)
#define StaticAssertExpr(condition, errmessage) \
	((void) ({ StaticAssertStmt(condition, errmessage); true; }))
#define StaticAssertDecl(condition, errmessage) \
	_Static_assert(condition, errmessage)

#define Max(x, y)		((x) > (y) ? (x) : (y))
#define Min(x, y)		((x) < (y) ? (x) : (y))

#define TYPEALIGN(ALIGNVAL, LEN)  \
	(((uintptr_t) (LEN) + ((ALIGNVAL) - 1)) & ~((uintptr_t) ((ALIGNVAL) - 1)))
#define MAXIMUM_ALIGNOF 8
#define MAXALIGN(LEN) TYPEALIGN(MAXIMUM_ALIGNOF, (LEN))

/*
 * MemSetAligned is a fast zero-fill for word-aligned, word-sized regions
 * (falls back to memset() outside that fast path).
 */
#define MEMSET_LOOP_LIMIT 1024
#define LONG_ALIGN_MASK (sizeof(long) - 1)

#define MemSetAligned(start, val, len) \
	do { \
		long   *_start = (long *) (start); \
		int		_val = (val); \
		Size	_len = (len); \
		if ((_len & LONG_ALIGN_MASK) == 0 && \
			_val == 0 && \
			_len <= MEMSET_LOOP_LIMIT && \
			MEMSET_LOOP_LIMIT != 0) \
		{ \
			long *_stop = (long *) ((char *) _start + _len); \
			while (_start < _stop) \
				*_start++ = 0; \
		} \
		else \
			memset(_start, _val, _len); \
	} while (0)

/*
 * pg_leftmost_one_pos32
 *		Returns the position of the most significant set bit in "word",
 *		measured from the least significant bit.  word must not be 0.
 */
static inline int
pg_leftmost_one_pos32(uint32 word)
{
	Assert(word != 0);
	return 31 - __builtin_clz(word);
}

/*
 * Overflow-checked Size arithmetic.
 */
static inline bool
pg_add_size_overflow(Size a, Size b, Size *result)
{
	return __builtin_add_overflow(a, b, result);
}

static inline bool
pg_mul_size_overflow(Size a, Size b, Size *result)
{
	return __builtin_mul_overflow(a, b, result);
}

/*
 * NodeTag identifies which MemoryContext implementation a given context
 * is. Only AllocSetContext is implemented; the others are reserved so
 * that implementing them later doesn't renumber this enum.
 */
typedef enum NodeTag
{
	T_AllocSetContext = 1,
	T_SlabContext,
	T_GenerationContext,
	T_BumpContext
} NodeTag;

/*
 * Every file that needs MemoryContext or MemoryContextCallback starts by
 * including this header (directly or transitively), so the opaque-pointer
 * forward declarations live here rather than in palloc.h.
 */
typedef struct MemoryContextData *MemoryContext;
typedef struct MemoryContextCallback MemoryContextCallback;

#endif							/* PG_COMPAT_H */
