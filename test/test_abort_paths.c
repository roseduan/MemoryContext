/*
 * The abort()-on-failure contract documented in README: a non-NO_OOM
 * allocation failure or an invalid size must terminate the process (not
 * return NULL), while MCXT_ALLOC_NO_OOM must still cleanly return NULL.
 *
 * These paths can't be exercised in-process -- abort() would take the
 * whole test binary down with it -- so each one runs in a forked child.
 * The child inherits TopMemoryContext/CurrentMemoryContext already set
 * up by the parent's one-time MemoryContextInit() call (see test_main.c);
 * it must NOT call MemoryContextInit() again itself, since fork() already
 * gave it a complete, valid copy of that state.
 *
 * Under AddressSanitizer, the two real-OOM tests need
 * ASAN_OPTIONS=allocator_may_return_null=1. By default ASan treats an
 * allocation request above its own internal cap as a bug worth crashing
 * on -- it prints its own report and _exit()s with code 1 *instead of*
 * letting malloc() return NULL, which preempts this library's own
 * NULL-check-and-abort() path before it ever runs. That env var restores
 * malloc()'s normal "return NULL" contract so what's actually under test
 * here -- this library's own handling of that NULL -- runs the same way
 * it does in a plain build. This isn't a workaround for a bug; it's
 * telling ASan which layer's OOM contract this suite is testing.
 */
#include "test.h"

#include "mctx.h"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static int
run_in_child(void (*body) (void))
{
	pid_t		pid = fork();

	if (pid == 0)
	{
		/* Silence the expected stderr diagnostic so it doesn't clutter
		 * this suite's output; the diagnostic text itself isn't what's
		 * under test here, just the resulting abort()/return. */
		freopen("/dev/null", "w", stderr);
		body();
		_exit(0);				/* only reached if body() didn't abort */
	}

	int			status;

	waitpid(pid, &status, 0);
	return status;
}

static void
child_invalid_size(void)
{
	palloc((Size) -1);
}

TEST(test_invalid_size_aborts_the_process)
{
	int			status = run_in_child(child_invalid_size);

	CHECK(WIFSIGNALED(status));
	if (WIFSIGNALED(status))
		CHECK(WTERMSIG(status) == SIGABRT);
}

/*
 * SIZE_MAX/4 is safely under AllocHugeSizeIsValid's SIZE_MAX/2 cap (so it
 * reaches the real malloc() call instead of failing the size-validity
 * check), but vastly exceeds any real or virtualized 64-bit system's
 * entire address space (a few dozen petabytes at most) -- not just more
 * memory than is physically installed, which overcommit could paper
 * over, but more than the CPU can even address. A smaller "obviously
 * too big" constant like 1 TB isn't reliable here: it's well within
 * reach of a plain mmap() reservation under Linux's default overcommit
 * policy, and sanitizer-instrumented allocators in particular are prone
 * to actually handing that back rather than failing.
 */
#define UNSATISFIABLE_SIZE ((Size) -1 / 4)

static void
child_oom_without_no_oom_flag(void)
{
	palloc_extended(UNSATISFIABLE_SIZE, MCXT_ALLOC_HUGE);
}

TEST(test_oom_without_no_oom_flag_aborts_the_process)
{
	int			status = run_in_child(child_oom_without_no_oom_flag);

	CHECK(WIFSIGNALED(status));
	if (WIFSIGNALED(status))
		CHECK(WTERMSIG(status) == SIGABRT);
}

static void
child_oom_with_no_oom_flag(void)
{
	void	   *p = palloc_extended(UNSATISFIABLE_SIZE,
									MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM);

	_exit(p == NULL ? 0 : 1);
}

TEST(test_no_oom_flag_returns_null_instead_of_aborting)
{
	int			status = run_in_child(child_oom_with_no_oom_flag);

	CHECK(WIFEXITED(status));
	if (WIFEXITED(status))
		CHECK(WEXITSTATUS(status) == 0);
}

int
run_abort_path_tests(void)
{
	int			run = 0,
				failed = 0;

	printf("out-of-memory / invalid-size failure paths (forked)\n");
	RUN_TEST(run, failed, test_invalid_size_aborts_the_process);
	RUN_TEST(run, failed, test_oom_without_no_oom_flag_aborts_the_process);
	RUN_TEST(run, failed, test_no_oom_flag_returns_null_instead_of_aborting);
	printf("  %d/%d passed\n\n", run - failed, run);

	return failed;
}
