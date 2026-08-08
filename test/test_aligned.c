/*
 * palloc_aligned()/repalloc_extended(): alignment guarantee, data
 * preservation across realloc, and that the redirect-chunk machinery
 * routes GetMemoryChunkContext/GetMemoryChunkSpace correctly.
 */
#include "test.h"

#include "mctx.h"

#include <stdint.h>
#include <string.h>

TEST(test_palloc_aligned_returns_aligned_pointer)
{
	void	   *p = palloc_aligned(37, 64, 0);

	CHECK(p != NULL);
	CHECK(((uintptr_t) p & 63) == 0);

	memset(p, 0xAB, 37);
	CHECK(((unsigned char *) p)[36] == 0xAB);

	pfree(p);
}

TEST(test_palloc_aligned_chunk_reports_owning_context)
{
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "aligned",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(ctx);

	void	   *p = palloc_aligned(16, 32, 0);

	CHECK(GetMemoryChunkContext(p) == ctx);
	CHECK(GetMemoryChunkSpace(p) > 0);

	pfree(p);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(ctx);
}

TEST(test_repalloc_extended_preserves_data_and_alignment)
{
	void	   *p = palloc_aligned(37, 64, 0);

	memset(p, 0xAB, 37);

	p = repalloc_extended(p, 200, 0);

	CHECK(p != NULL);
	CHECK(((uintptr_t) p & 63) == 0);
	CHECK(((unsigned char *) p)[36] == 0xAB);

	pfree(p);
}

TEST(test_alignto_below_maxalign_falls_back_to_plain_alloc)
{
	/* alignto <= MAXIMUM_ALIGNOF should just be a normal allocation, not
	 * a redirect chunk -- exercised here via the public API only, since
	 * that distinction is an internal implementation detail. */
	void	   *p = palloc_aligned(16, 8, 0);

	CHECK(p != NULL);
	CHECK(((uintptr_t) p & 7) == 0);
	pfree(p);
}

int
run_aligned_tests(void)
{
	int			run = 0,
				failed = 0;

	printf("palloc_aligned / repalloc_extended\n");
	RUN_TEST(run, failed, test_palloc_aligned_returns_aligned_pointer);
	RUN_TEST(run, failed, test_palloc_aligned_chunk_reports_owning_context);
	RUN_TEST(run, failed, test_repalloc_extended_preserves_data_and_alignment);
	RUN_TEST(run, failed, test_alignto_below_maxalign_falls_back_to_plain_alloc);
	printf("  %d/%d passed\n\n", run - failed, run);

	return failed;
}
