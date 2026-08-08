/*
 * The palloc/pfree/repalloc API surface: zeroing, growth across the
 * small/large chunk boundary, repalloc0's exact-tail-zeroing contract,
 * palloc(0), the strdup family, and the overflow-checked size arithmetic
 * plus the type-safe allocation macros built on top of it.
 */
#include "test.h"

#include "mctx.h"

#include <string.h>

TEST(test_palloc0_zeroes_the_whole_allocation)
{
	char	   *p = palloc0(16);

	for (int i = 0; i < 16; i++)
		CHECK(p[i] == 0);
	pfree(p);
}

TEST(test_palloc_zero_size_is_valid)
{
	void	   *p = palloc(0);

	CHECK(p != NULL);
	pfree(p);
}

TEST(test_repalloc_grows_across_small_to_large_boundary)
{
	char	   *buf = palloc(4);

	memcpy(buf, "abcd", 4);

	/* ALLOCSET_SEPARATE_THRESHOLD is 8192; this forces the dedicated-block
	 * ("external chunk") path */
	buf = repalloc(buf, 64 * 1024);

	CHECK(buf != NULL);
	CHECK(memcmp(buf, "abcd", 4) == 0);

	pfree(buf);
}

TEST(test_repalloc_shrinks_within_same_chunk)
{
	char	   *buf = palloc(200);

	memcpy(buf, "shrink-me", 9);
	buf = repalloc(buf, 32);

	CHECK(buf != NULL);
	CHECK(memcmp(buf, "shrink-me", 9) == 0);

	pfree(buf);
}

TEST(test_repalloc0_zeroes_only_the_new_tail)
{
	char	   *buf = palloc(4);

	memcpy(buf, "abcd", 4);
	buf = repalloc0(buf, 4, 8);

	CHECK(memcmp(buf, "abcd", 4) == 0);
	CHECK(buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0);

	pfree(buf);
}

TEST(test_strdup_family)
{
	char	   *dup = pstrdup("postgres memory contexts");

	CHECK(strcmp(dup, "postgres memory contexts") == 0);
	pfree(dup);

	char	   *n = pnstrdup("abcdef", 3);

	CHECK(strcmp(n, "abc") == 0);
	pfree(n);

	char	   *chomped = pchomp("line\n\n");

	CHECK(strcmp(chomped, "line") == 0);
	pfree(chomped);
}

TEST(test_add_size_and_mul_size_normal_path)
{
	CHECK(add_size(3, 4) == 7);
	CHECK(mul_size(3, 4) == 12);
	CHECK(mul_size(0, 12345) == 0);
}

TEST(test_alloc_huge_and_repalloc_huge)
{
	/* MemoryContextAllocHuge/repalloc_huge take the AllocHugeSizeIsValid
	 * path (MaxAllocHugeSize, ~SIZE_MAX/2) instead of the normal
	 * AllocSizeIsValid cap (~1GB) -- exercised here at a modest size, since
	 * the size cap itself only differs once you're past ~1GB, which isn't
	 * worth actually allocating in a unit test. */
	char	   *buf = MemoryContextAllocHuge(TopMemoryContext, 4096);

	CHECK(buf != NULL);
	memset(buf, 0xCD, 4096);

	buf = repalloc_huge(buf, 8192);
	CHECK(buf != NULL);
	CHECK((unsigned char) buf[4095] == 0xCD);

	pfree(buf);
}

TEST(test_alloc_extended_zero_flag)
{
	char	   *buf = MemoryContextAllocExtended(TopMemoryContext, 64,
												 MCXT_ALLOC_ZERO);
	bool		all_zero = true;

	CHECK(buf != NULL);
	for (int i = 0; i < 64; i++)
		if (buf[i] != 0)
			all_zero = false;
	CHECK(all_zero);

	pfree(buf);
}

TEST(test_palloc_mul_family)
{
	int		   *arr = palloc_mul(sizeof(int), 10);

	for (int i = 0; i < 10; i++)
		arr[i] = i;
	CHECK(arr[9] == 9);
	pfree(arr);

	int		   *zeroed = palloc0_mul(sizeof(int), 10);
	bool		all_zero = true;

	for (int i = 0; i < 10; i++)
		if (zeroed[i] != 0)
			all_zero = false;
	CHECK(all_zero);
	pfree(zeroed);

	int		   *ext = palloc_mul_extended(sizeof(int), 10, MCXT_ALLOC_ZERO);

	CHECK(ext != NULL);
	CHECK(ext[0] == 0);
	pfree(ext);
}

TEST(test_type_safe_allocation_macros)
{
	int		   *one = palloc_object(int);

	*one = 7;
	CHECK(*one == 7);
	pfree(one);

	int		   *zeroed = palloc0_object(int);

	CHECK(*zeroed == 0);
	pfree(zeroed);

	int		   *arr = palloc_array(int, 10);

	for (int i = 0; i < 10; i++)
		arr[i] = i;
	CHECK(arr[9] == 9);
	pfree(arr);

	int		   *arr0 = palloc0_array(int, 10);
	bool		all_zero = true;

	for (int i = 0; i < 10; i++)
		if (arr0[i] != 0)
			all_zero = false;
	CHECK(all_zero);
	pfree(arr0);
}

int
run_palloc_tests(void)
{
	int			run = 0,
				failed = 0;

	printf("palloc/pfree/repalloc API\n");
	RUN_TEST(run, failed, test_palloc0_zeroes_the_whole_allocation);
	RUN_TEST(run, failed, test_palloc_zero_size_is_valid);
	RUN_TEST(run, failed, test_repalloc_grows_across_small_to_large_boundary);
	RUN_TEST(run, failed, test_repalloc_shrinks_within_same_chunk);
	RUN_TEST(run, failed, test_repalloc0_zeroes_only_the_new_tail);
	RUN_TEST(run, failed, test_strdup_family);
	RUN_TEST(run, failed, test_alloc_huge_and_repalloc_huge);
	RUN_TEST(run, failed, test_alloc_extended_zero_flag);
	RUN_TEST(run, failed, test_palloc_mul_family);
	RUN_TEST(run, failed, test_add_size_and_mul_size_normal_path);
	RUN_TEST(run, failed, test_type_safe_allocation_macros);
	printf("  %d/%d passed\n\n", run - failed, run);

	return failed;
}
