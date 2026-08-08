/*
 * Context tree and lifecycle: create/reset/delete, reset callbacks
 * (ordering, unregistration), reparenting, memory accounting.
 *
 * Each test creates its own child context under TopMemoryContext and
 * deletes it before returning, so tests don't leak state into each other
 * -- TopMemoryContext/CurrentMemoryContext themselves are process-global
 * (MemoryContextInit() runs once, in test_main.c).
 */
#include "test.h"

#include "mctx.h"

#include <stdint.h>
#include <string.h>

static int	reset_calls;
static int	reset_order[8];
static int	reset_order_len;

static void
record_reset(void *arg)
{
	reset_order[reset_order_len++] = (int) (intptr_t) arg;
	reset_calls++;
}

TEST(test_create_switch_and_delete)
{
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "t1",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(ctx);

	CHECK(CurrentMemoryContext == ctx);
	CHECK(MemoryContextGetParent(ctx) == TopMemoryContext);

	MemoryContextSwitchTo(old);
	CHECK(CurrentMemoryContext == old);

	MemoryContextDelete(ctx);
}

TEST(test_reset_deletes_children_but_context_survives)
{
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "t2",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(ctx);

	AllocSetContextCreate(ctx, "child", ALLOCSET_SMALL_SIZES);
	palloc(64);

	MemoryContextReset(ctx);

	CHECK(ctx->firstchild == NULL);	/* child was deleted, not just reset */
	CHECK(CurrentMemoryContext == ctx); /* ctx itself survives its own reset */

	MemoryContextSwitchTo(old);
	MemoryContextDelete(ctx);
}

TEST(test_reset_callbacks_run_children_first_and_lifo)
{
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "t3",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(ctx);
	MemoryContext child = AllocSetContextCreate(ctx, "child",
												ALLOCSET_SMALL_SIZES);

	reset_order_len = 0;
	reset_calls = 0;

	MemoryContextCallback *cb1 = MemoryContextAlloc(ctx, sizeof(*cb1));

	cb1->func = record_reset;
	cb1->arg = (void *) (intptr_t) 1;
	MemoryContextRegisterResetCallback(ctx, cb1);

	/* registered after cb1 on the same context -> must fire before it */
	MemoryContextCallback *cb2 = MemoryContextAlloc(ctx, sizeof(*cb2));

	cb2->func = record_reset;
	cb2->arg = (void *) (intptr_t) 2;
	MemoryContextRegisterResetCallback(ctx, cb2);

	/* on the child -> must fire before either of ctx's own callbacks */
	MemoryContextCallback *cb3 = MemoryContextAlloc(child, sizeof(*cb3));

	cb3->func = record_reset;
	cb3->arg = (void *) (intptr_t) 3;
	MemoryContextRegisterResetCallback(child, cb3);

	MemoryContextReset(ctx);

	CHECK(reset_calls == 3);
	CHECK(reset_order_len == 3);
	if (reset_order_len == 3)
	{
		CHECK(reset_order[0] == 3);
		CHECK(reset_order[1] == 2);
		CHECK(reset_order[2] == 1);
	}

	MemoryContextSwitchTo(old);
	MemoryContextDelete(ctx);
}

TEST(test_unregister_reset_callback_prevents_it_firing)
{
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "t4",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(ctx);

	reset_calls = 0;

	MemoryContextCallback *cb = MemoryContextAlloc(ctx, sizeof(*cb));

	cb->func = record_reset;
	cb->arg = NULL;
	MemoryContextRegisterResetCallback(ctx, cb);
	MemoryContextUnregisterResetCallback(ctx, cb);

	MemoryContextResetOnly(ctx);
	CHECK(reset_calls == 0);

	MemoryContextSwitchTo(old);
	MemoryContextDelete(ctx);
}

TEST(test_reparent_moves_context_to_new_parent)
{
	MemoryContext a = AllocSetContextCreate(TopMemoryContext, "a",
											ALLOCSET_SMALL_SIZES);
	MemoryContext b = AllocSetContextCreate(TopMemoryContext, "b",
											ALLOCSET_SMALL_SIZES);
	MemoryContext c = AllocSetContextCreate(a, "c", ALLOCSET_SMALL_SIZES);

	CHECK(MemoryContextGetParent(c) == a);

	MemoryContextSetParent(c, b);
	CHECK(MemoryContextGetParent(c) == b);

	MemoryContextDelete(a);		/* must not take c down with it anymore */
	CHECK(MemoryContextGetParent(c) == b);

	MemoryContextDelete(b);		/* now this takes c down */
}

TEST(test_reset_children_resets_without_deleting)
{
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "t6",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext child1 = AllocSetContextCreate(ctx, "child1",
												 ALLOCSET_SMALL_SIZES);
	MemoryContext child2 = AllocSetContextCreate(ctx, "child2",
												 ALLOCSET_SMALL_SIZES);

	reset_calls = 0;

	MemoryContextCallback *cb1 = MemoryContextAlloc(child1, sizeof(*cb1));

	cb1->func = record_reset;
	cb1->arg = NULL;
	MemoryContextRegisterResetCallback(child1, cb1);

	MemoryContextCallback *cb2 = MemoryContextAlloc(child2, sizeof(*cb2));

	cb2->func = record_reset;
	cb2->arg = NULL;
	MemoryContextRegisterResetCallback(child2, cb2);

	MemoryContextResetChildren(ctx);

	/* both children's callbacks fired (they were reset)... */
	CHECK(reset_calls == 2);
	/* ...but the children themselves still exist as children of ctx,
	 * unlike MemoryContextReset(ctx), which would have deleted them */
	CHECK(ctx->firstchild != NULL);
	CHECK(MemoryContextGetParent(child1) == ctx);
	CHECK(MemoryContextGetParent(child2) == ctx);

	MemoryContextDelete(ctx);
}

TEST(test_set_identifier)
{
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "t7",
											  ALLOCSET_SMALL_SIZES);

	MemoryContextSetIdentifier(ctx, "my-identifier");
	CHECK(ctx->ident != NULL);
	if (ctx->ident != NULL)
		CHECK(strcmp(ctx->ident, "my-identifier") == 0);

	MemoryContextSetIdentifier(ctx, NULL);
	CHECK(ctx->ident == NULL);

	MemoryContextDelete(ctx);
}

TEST(test_is_empty_tracks_allocation_state)
{
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "t8",
											  ALLOCSET_SMALL_SIZES);

	CHECK(MemoryContextIsEmpty(ctx));

	MemoryContextAlloc(ctx, 16);
	CHECK(!MemoryContextIsEmpty(ctx));

	MemoryContextResetOnly(ctx);
	CHECK(MemoryContextIsEmpty(ctx));

	MemoryContextDelete(ctx);
}

TEST(test_context_recycling_freelist_survives_overflow)
{
	/* AllocSetContextCreate's freelist (context_freelists[]) caps at 100
	 * recycled headers per size class; creating and deleting more than
	 * that in a row exercises both the "recycle it" path and the
	 * "freelist is full, just discard it" path. */
	for (int i = 0; i < 150; i++)
	{
		MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "recycled",
												  ALLOCSET_DEFAULT_SIZES);

		MemoryContextAlloc(ctx, 32);
		MemoryContextDelete(ctx);
	}

	/* the library must still work correctly afterward */
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "after",
											  ALLOCSET_DEFAULT_SIZES);
	int		   *p = MemoryContextAlloc(ctx, sizeof(int));

	*p = 42;
	CHECK(*p == 42);

	MemoryContextDelete(ctx);
}

TEST(test_mem_allocated_reflects_allocations)
{
	MemoryContext ctx = AllocSetContextCreate(TopMemoryContext, "t5",
											  ALLOCSET_SMALL_SIZES);
	Size		before = MemoryContextMemAllocated(ctx, false);

	MemoryContextAlloc(ctx, 4096);

	Size		after = MemoryContextMemAllocated(ctx, false);

	CHECK(after > before);

	MemoryContextCounters counters;

	MemoryContextMemConsumed(ctx, &counters);
	CHECK(counters.totalspace > 0);

	MemoryContextDelete(ctx);
}

int
run_mcxt_tests(void)
{
	int			run = 0,
				failed = 0;

	printf("mcxt.c -- context tree & lifecycle\n");
	RUN_TEST(run, failed, test_create_switch_and_delete);
	RUN_TEST(run, failed, test_reset_deletes_children_but_context_survives);
	RUN_TEST(run, failed, test_reset_callbacks_run_children_first_and_lifo);
	RUN_TEST(run, failed, test_unregister_reset_callback_prevents_it_firing);
	RUN_TEST(run, failed, test_reparent_moves_context_to_new_parent);
	RUN_TEST(run, failed, test_reset_children_resets_without_deleting);
	RUN_TEST(run, failed, test_set_identifier);
	RUN_TEST(run, failed, test_is_empty_tracks_allocation_state);
	RUN_TEST(run, failed, test_context_recycling_freelist_survives_overflow);
	RUN_TEST(run, failed, test_mem_allocated_reflects_allocations);
	printf("  %d/%d passed\n\n", run - failed, run);

	return failed;
}
