#include "mctx.h"

#include <stdio.h>
#include <sys/resource.h>

int			run_mcxt_tests(void);
int			run_palloc_tests(void);
int			run_aligned_tests(void);
int			run_abort_path_tests(void);

int
main(void)
{
	int			total_failed = 0;
	struct rlimit no_core = {0, 0};

	/* test_abort_paths.c deliberately triggers SIGABRT in forked children
	 * -- that's the point of that suite, not a crash to leave a core file
	 * about. setrlimit() is inherited across fork(), so this covers them
	 * too. Best-effort: if it fails (e.g. a hard limit already caps this
	 * below zero, which can't happen, or a sandboxed environment denies
	 * setrlimit outright), the tests still pass or fail the same way --
	 * worst case some stray core-* files show up, same as before. */
	setrlimit(RLIMIT_CORE, &no_core);

	/* Once per process -- everything else runs under a per-test child
	 * context, never against TopMemoryContext itself. */
	MemoryContextInit();

	total_failed += run_mcxt_tests();
	total_failed += run_palloc_tests();
	total_failed += run_aligned_tests();
	total_failed += run_abort_path_tests();

	if (total_failed == 0)
		printf("all tests passed\n");
	else
		printf("%d test(s) FAILED\n", total_failed);

	return total_failed == 0 ? 0 : 1;
}
