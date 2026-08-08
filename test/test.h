#ifndef TEST_H
#define TEST_H

#include <stdio.h>

/*
 * Minimal xUnit-style test macros -- no external test framework, matching
 * this library's zero-dependency stance.
 *
 * A failing CHECK marks the current test failed and lets it keep running
 * (it does not abort the process), so one wrong assertion doesn't hide
 * whatever else that test -- or the rest of the suite -- would have
 * reported.
 */

#define TEST(name) static void name(int *_failed)

#define CHECK(cond) \
	do { \
		if (!(cond)) \
		{ \
			printf("\n    %s:%d: CHECK(%s) failed", __FILE__, __LINE__, #cond); \
			(*_failed)++; \
		} \
	} while (0)

#define RUN_TEST(run_counter, failed_counter, name) \
	do { \
		int _one_failed = 0; \
		printf("  %-58s", #name); \
		name(&_one_failed); \
		if (_one_failed) \
		{ \
			printf(" FAILED\n"); \
			(failed_counter)++; \
		} \
		else \
			printf("ok\n"); \
		(run_counter)++; \
	} while (0)

#endif							/* TEST_H */
