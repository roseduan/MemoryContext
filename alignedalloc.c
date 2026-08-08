/*-------------------------------------------------------------------------
 *
 * alignedalloc.c
 *	  Allocator functions to implement palloc_aligned
 *
 * This is not a fully-fledged MemoryContext type as there is no means to
 * create a MemoryContext of this type.  The code here only serves to allow
 * operations such as pfree() and repalloc() to work correctly on a memory
 * chunk that was allocated by palloc_aligned().
 *
 * Portions Copyright (c) 2022-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>

#include "mctx_internal.h"
#include "memdebug.h"
#include "memutils.h"
#include "memutils_memorychunk.h"
#include "palloc.h"

/*
 * AlignedAllocFree
 *		Frees allocated memory; memory is removed from its owning context.
 */
void
AlignedAllocFree(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	void	   *unaligned;

	VALGRIND_MAKE_MEM_DEFINED(chunk, sizeof(MemoryChunk));

	Assert(!MemoryChunkIsExternal(chunk));

	/* obtain the original (unaligned) allocated pointer */
	unaligned = MemoryChunkGetBlock(chunk);

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (!sentinel_ok(pointer, chunk->requested_size))
		elog(WARNING, "detected write past chunk end in %s %p",
			 GetMemoryChunkContext(unaligned)->name, chunk);
#endif

	/*
	 * Create a dummy vchunk covering the start of the unaligned chunk, but
	 * not overlapping the aligned chunk.  This will be freed while pfree'ing
	 * the unaligned chunk, keeping Valgrind happy.
	 */
	VALGRIND_MEMPOOL_ALLOC(GetMemoryChunkContext(unaligned), unaligned,
						   (char *) pointer - (char *) unaligned);

	/* Recursively pfree the unaligned chunk */
	pfree(unaligned);
}

/*
 * AlignedAllocRealloc
 *		Change the allocated size of a chunk and return possibly a different
 *		pointer to a memory address aligned to the same boundary as the
 *		originally requested alignment.  The contents of 'pointer' will be
 *		copied into the returned pointer up until 'size'.  Any additional
 *		memory will be uninitialized.
 */
void *
AlignedAllocRealloc(void *pointer, Size size, int flags)
{
	MemoryChunk *redirchunk = PointerGetMemoryChunk(pointer);
	Size		alignto;
	void	   *unaligned;
	MemoryContext ctx;
	Size		old_size;
	void	   *newptr;

	VALGRIND_MAKE_MEM_DEFINED(redirchunk, sizeof(MemoryChunk));

	alignto = MemoryChunkGetValue(redirchunk);
	unaligned = MemoryChunkGetBlock(redirchunk);

	/* sanity check this is a power of 2 value */
	Assert((alignto & (alignto - 1)) == 0);

	/*
	 * Determine the size of the original allocation. This may be an
	 * overestimate (GetMemoryChunkSpace() rounds up), which is fine since
	 * it's only used to bound the memcpy() below.
	 */
	old_size = GetMemoryChunkSpace(unaligned) -
		PallocAlignedExtraBytes(alignto) - sizeof(MemoryChunk);

#ifdef MEMORY_CONTEXT_CHECKING
	Assert(old_size >= redirchunk->requested_size);
#endif

	/*
	 * To keep things simple, we always allocate a new aligned chunk and copy
	 * data into it.
	 */
	ctx = GetMemoryChunkContext(unaligned);
	newptr = MemoryContextAllocAligned(ctx, size, alignto, flags);

	/* Cope cleanly with OOM */
	if (unlikely(newptr == NULL))
	{
		VALGRIND_MAKE_MEM_NOACCESS(redirchunk, sizeof(MemoryChunk));
		return MemoryContextAllocationFailure(ctx, size, flags);
	}

	VALGRIND_MAKE_MEM_DEFINED(pointer, old_size);
	memcpy(newptr, pointer, Min(size, old_size));

	/*
	 * Create a dummy vchunk covering the start of the old unaligned chunk,
	 * but not overlapping the aligned chunk. This keeps Valgrind happy.
	 */
	VALGRIND_MEMPOOL_ALLOC(ctx, unaligned,
						   (char *) pointer - (char *) unaligned);

	pfree(unaligned);

	return newptr;
}

/*
 * AlignedAllocGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
AlignedAllocGetChunkContext(void *pointer)
{
	MemoryChunk *redirchunk = PointerGetMemoryChunk(pointer);
	MemoryContext cxt;

	VALGRIND_MAKE_MEM_DEFINED(redirchunk, sizeof(MemoryChunk));

	Assert(!MemoryChunkIsExternal(redirchunk));

	cxt = GetMemoryChunkContext(MemoryChunkGetBlock(redirchunk));

	VALGRIND_MAKE_MEM_NOACCESS(redirchunk, sizeof(MemoryChunk));

	return cxt;
}

/*
 * AlignedAllocGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
Size
AlignedAllocGetChunkSpace(void *pointer)
{
	MemoryChunk *redirchunk = PointerGetMemoryChunk(pointer);
	void	   *unaligned;
	Size		space;

	VALGRIND_MAKE_MEM_DEFINED(redirchunk, sizeof(MemoryChunk));

	unaligned = MemoryChunkGetBlock(redirchunk);
	space = GetMemoryChunkSpace(unaligned);

	VALGRIND_MAKE_MEM_NOACCESS(redirchunk, sizeof(MemoryChunk));

	return space;
}
