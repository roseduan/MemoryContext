/*-------------------------------------------------------------------------
 *
 * mcxt.c
 *	  POSTGRES memory context management code.
 *
 * This module handles context management operations that are independent
 * of the particular kind of context being operated on.  It calls
 * context-type-specific operations via the function pointers in a
 * context's MemoryContextMethods struct.
 *
 * A note about Valgrind support: when USE_VALGRIND is defined, we provide
 * support for memory leak tracking at the allocation-unit level.  Valgrind
 * does leak detection by tracking allocated "chunks", which can be grouped
 * into "pools".  The "chunk" terminology is overloaded, since we use that
 * word for our allocation units, and it's sometimes important to distinguish
 * those from the Valgrind objects that describe them.  To reduce confusion,
 * let's use the terms "vchunk" and "vpool" for the Valgrind objects.
 *
 * We use a separate vpool for each memory context.  The context-type-specific
 * code is responsible for creating and deleting the vpools, and also for
 * creating vchunks to cover its management data structures such as block
 * headers.  (There must be a vchunk that includes every pointer we want
 * Valgrind to consider for leak-tracking purposes.)  This module creates
 * and deletes the vchunks that cover the caller-visible allocated chunks.
 * However, the context-type-specific code must handle cleaning up those
 * vchunks too during memory context reset operations.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

/* strnlen() is POSIX, not C11; needed since we build with -std=c11. Must
 * precede every #include -- glibc only honors this at first inclusion. */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <string.h>

#include "mctx_internal.h"
#include "memdebug.h"
#include "memutils.h"
#include "memutils_memorychunk.h"
#include "palloc.h"


static void BogusFree(void *pointer);
static void *BogusRealloc(void *pointer, Size size, int flags);
static MemoryContext BogusGetChunkContext(void *pointer);
static Size BogusGetChunkSpace(void *pointer);

/*****************************************************************************
 *	  GLOBAL MEMORY															 *
 *****************************************************************************/
#define BOGUS_MCTX(id) \
	[id].free_p = BogusFree, \
	[id].realloc = BogusRealloc, \
	[id].get_chunk_context = BogusGetChunkContext, \
	[id].get_chunk_space = BogusGetChunkSpace

static const MemoryContextMethods mcxt_methods[] = {
	/* aset.c */
	[MCTX_ASET_ID].alloc = AllocSetAlloc,
	[MCTX_ASET_ID].free_p = AllocSetFree,
	[MCTX_ASET_ID].realloc = AllocSetRealloc,
	[MCTX_ASET_ID].reset = AllocSetReset,
	[MCTX_ASET_ID].delete_context = AllocSetDelete,
	[MCTX_ASET_ID].get_chunk_context = AllocSetGetChunkContext,
	[MCTX_ASET_ID].get_chunk_space = AllocSetGetChunkSpace,
	[MCTX_ASET_ID].is_empty = AllocSetIsEmpty,
	[MCTX_ASET_ID].stats = AllocSetStats,
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_ASET_ID].check = AllocSetCheck,
#endif

	/* alignedalloc.c */
	[MCTX_ALIGNED_REDIRECT_ID].alloc = NULL,	/* not required */
	[MCTX_ALIGNED_REDIRECT_ID].free_p = AlignedAllocFree,
	[MCTX_ALIGNED_REDIRECT_ID].realloc = AlignedAllocRealloc,
	[MCTX_ALIGNED_REDIRECT_ID].reset = NULL,	/* not required */
	[MCTX_ALIGNED_REDIRECT_ID].delete_context = NULL,	/* not required */
	[MCTX_ALIGNED_REDIRECT_ID].get_chunk_context = AlignedAllocGetChunkContext,
	[MCTX_ALIGNED_REDIRECT_ID].get_chunk_space = AlignedAllocGetChunkSpace,
	[MCTX_ALIGNED_REDIRECT_ID].is_empty = NULL, /* not required */
	[MCTX_ALIGNED_REDIRECT_ID].stats = NULL,	/* not required */
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_ALIGNED_REDIRECT_ID].check = NULL,	/* not required */
#endif

	/*
	 * Reserved and unused IDs should have dummy entries here.  This allows us
	 * to fail cleanly if a bogus pointer is passed to pfree or the like.
	 * Generation/Slab/Bump are not implemented, so their IDs are routed here
	 * too.
	 */
	BOGUS_MCTX(MCTX_1_RESERVED_GLIBC_ID),
	BOGUS_MCTX(MCTX_2_RESERVED_GLIBC_ID),
	BOGUS_MCTX(MCTX_GENERATION_ID),
	BOGUS_MCTX(MCTX_SLAB_ID),
	BOGUS_MCTX(MCTX_BUMP_ID),
	BOGUS_MCTX(MCTX_8_UNUSED_ID),
	BOGUS_MCTX(MCTX_9_UNUSED_ID),
	BOGUS_MCTX(MCTX_10_UNUSED_ID),
	BOGUS_MCTX(MCTX_11_UNUSED_ID),
	BOGUS_MCTX(MCTX_12_UNUSED_ID),
	BOGUS_MCTX(MCTX_13_UNUSED_ID),
	BOGUS_MCTX(MCTX_14_UNUSED_ID),
	BOGUS_MCTX(MCTX_0_RESERVED_UNUSEDMEM_ID),
	BOGUS_MCTX(MCTX_15_RESERVED_WIPEDMEM_ID)
};

#undef BOGUS_MCTX

/*
 * CurrentMemoryContext
 *		Default memory context for allocations.
 */
MemoryContext CurrentMemoryContext = NULL;

/*
 * Standard top-level contexts. For a description of the purpose of each
 * of these contexts, refer to README. Only TopMemoryContext/
 * CurrentMemoryContext/ErrorContext are ever set here -- the rest are
 * declared for API completeness but nothing populates them.
 */
MemoryContext TopMemoryContext = NULL;
MemoryContext ErrorContext = NULL;
MemoryContext PostmasterContext = NULL;
MemoryContext CacheMemoryContext = NULL;
MemoryContext MessageContext = NULL;
MemoryContext TopTransactionContext = NULL;
MemoryContext CurTransactionContext = NULL;

/* This is a transient link to the active portal's memory context: */
MemoryContext PortalContext = NULL;

static void MemoryContextDeleteOnly(MemoryContext context);
static void MemoryContextCallResetCallbacks(MemoryContext context);

/*
 * Call the given function in the MemoryContextMethods for the memory context
 * type that 'pointer' belongs to.
 */
#define MCXT_METHOD(pointer, method) \
	mcxt_methods[GetMemoryChunkMethodID(pointer)].method

/*
 * GetMemoryChunkMethodID
 *		Return the MemoryContextMethodID from the uint64 chunk header which
 *		directly precedes 'pointer'.
 */
static inline MemoryContextMethodID
GetMemoryChunkMethodID(const void *pointer)
{
	uint64		header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer == (const void *) MAXALIGN(pointer));

	/* Allow access to the uint64 header */
	VALGRIND_MAKE_MEM_DEFINED((char *) pointer - sizeof(uint64), sizeof(uint64));

	header = *((const uint64 *) ((const char *) pointer - sizeof(uint64)));

	/* Disallow access to the uint64 header */
	VALGRIND_MAKE_MEM_NOACCESS((char *) pointer - sizeof(uint64), sizeof(uint64));

	return (MemoryContextMethodID) (header & MEMORY_CONTEXT_METHODID_MASK);
}

/*
 * GetMemoryChunkHeader
 *		Return the uint64 chunk header which directly precedes 'pointer'.
 *
 * This is only used after GetMemoryChunkMethodID, so no need for error checks.
 */
static inline uint64
GetMemoryChunkHeader(const void *pointer)
{
	uint64		header;

	VALGRIND_MAKE_MEM_DEFINED((char *) pointer - sizeof(uint64), sizeof(uint64));
	header = *((const uint64 *) ((const char *) pointer - sizeof(uint64)));
	VALGRIND_MAKE_MEM_NOACCESS((char *) pointer - sizeof(uint64), sizeof(uint64));

	return header;
}

/*
 * MemoryContextTraverseNext
 *		Helper function to traverse all descendants of a memory context
 *		without recursion.
 *
 * Recursion could lead to out-of-stack errors with deep context hierarchies,
 * which would be unpleasant in error cleanup code paths.
 *
 * To process 'context' and all its descendants, use a loop like this:
 *
 *     <process 'context'>
 *     for (MemoryContext curr = context->firstchild;
 *          curr != NULL;
 *          curr = MemoryContextTraverseNext(curr, context))
 *     {
 *         <process 'curr'>
 *     }
 *
 * This visits all the contexts in pre-order, that is a node is visited
 * before its children.
 */
static MemoryContext
MemoryContextTraverseNext(MemoryContext curr, MemoryContext top)
{
	/* After processing a node, traverse to its first child if any */
	if (curr->firstchild != NULL)
		return curr->firstchild;

	/*
	 * After processing a childless node, traverse to its next sibling if
	 * there is one.  If there isn't, traverse back up to the parent (which
	 * has already been visited, and now so have all its descendants).  We're
	 * done if that is "top", otherwise traverse to its next sibling if any,
	 * otherwise repeat moving up.
	 */
	while (curr->nextchild == NULL)
	{
		curr = curr->parent;
		if (curr == top)
			return NULL;
	}
	return curr->nextchild;
}

/*
 * Support routines to trap use of invalid memory context method IDs
 * (from calling pfree or the like on a bogus pointer).
 */
static void
BogusFree(void *pointer)
{
	mctx_report_corruption("pfree called with invalid pointer", pointer);
}

static void *
BogusRealloc(void *pointer, Size size, int flags)
{
	mctx_report_corruption("repalloc called with invalid pointer", pointer);
}

static MemoryContext
BogusGetChunkContext(void *pointer)
{
	mctx_report_corruption("GetMemoryChunkContext called with invalid pointer", pointer);
}

static Size
BogusGetChunkSpace(void *pointer)
{
	mctx_report_corruption("GetMemoryChunkSpace called with invalid pointer", pointer);
}


/*****************************************************************************
 *	  EXPORTED ROUTINES														 *
 *****************************************************************************/


/*
 * MemoryContextInit
 *		Start up the memory-context subsystem.
 *
 * This must be called before creating contexts or allocating memory in
 * contexts.  TopMemoryContext and ErrorContext are initialized here;
 * other contexts must be created afterwards.
 */
void
MemoryContextInit(void)
{
	Assert(TopMemoryContext == NULL);

	/*
	 * First, initialize TopMemoryContext, which is the parent of all others.
	 */
	TopMemoryContext = AllocSetContextCreate((MemoryContext) NULL,
											 "TopMemoryContext",
											 ALLOCSET_DEFAULT_SIZES);

	/*
	 * Not having any other place to point CurrentMemoryContext, make it point
	 * to TopMemoryContext.  Caller should change this soon!
	 */
	CurrentMemoryContext = TopMemoryContext;

	/*
	 * Initialize ErrorContext as an AllocSetContext with slow growth rate ---
	 * we don't really expect much to be allocated in it. More to the point,
	 * require it to contain at least 8K at all times. This is the only case
	 * where retained memory in a context is *essential* --- we want to be
	 * sure ErrorContext still has some memory even if we've run out
	 * elsewhere! Also, allow allocations in ErrorContext within a critical
	 * section.
	 */
	ErrorContext = AllocSetContextCreate(TopMemoryContext,
										 "ErrorContext",
										 8 * 1024,
										 8 * 1024,
										 8 * 1024);
	MemoryContextAllowInCriticalSection(ErrorContext, true);
}

/*
 * MemoryContextReset
 *		Release all space allocated within a context and delete all its
 *		descendant contexts (but not the named context itself).
 */
void
MemoryContextReset(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/* save a function call in common case where there are no children */
	if (context->firstchild != NULL)
		MemoryContextDeleteChildren(context);

	/* save a function call if no pallocs since startup or last reset */
	if (!context->isReset)
		MemoryContextResetOnly(context);
}

/*
 * MemoryContextResetOnly
 *		Release all space allocated within a context.
 *		Nothing is done to the context's descendant contexts.
 */
void
MemoryContextResetOnly(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/* Nothing to do if no pallocs since startup or last reset */
	if (!context->isReset)
	{
		MemoryContextCallResetCallbacks(context);
		context->methods->reset(context);
		context->isReset = true;
	}
}

/*
 * MemoryContextResetChildren
 *		Release all space allocated within a context's descendants,
 *		but don't delete the contexts themselves.  The named context
 *		itself is not touched.
 */
void
MemoryContextResetChildren(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	for (MemoryContext curr = context->firstchild;
		 curr != NULL;
		 curr = MemoryContextTraverseNext(curr, context))
	{
		MemoryContextResetOnly(curr);
	}
}

/*
 * MemoryContextDelete
 *		Delete a context and its descendants, and release all space
 *		allocated therein.
 *
 * The type-specific delete routine removes all storage for the context,
 * but we have to deal with descendant nodes here.
 */
void
MemoryContextDelete(MemoryContext context)
{
	MemoryContext curr;

	Assert(MemoryContextIsValid(context));

	/*
	 * Delete subcontexts from the bottom up.
	 *
	 * Note: Do not use recursion here.  A "stack depth limit exceeded" error
	 * would be unpleasant if we're already in the process of cleaning up from
	 * transaction abort.  We also cannot use MemoryContextTraverseNext() here
	 * because we modify the tree as we go.
	 */
	curr = context;
	for (;;)
	{
		MemoryContext parent;

		/* Descend down until we find a leaf context with no children */
		while (curr->firstchild != NULL)
			curr = curr->firstchild;

		/*
		 * We're now at a leaf with no children. Free it and continue from the
		 * parent.  Or if this was the original node, we're all done.
		 */
		parent = curr->parent;
		MemoryContextDeleteOnly(curr);

		if (curr == context)
			break;
		curr = parent;
	}
}

/*
 * Subroutine of MemoryContextDelete,
 * to delete a context that has no children.
 * We must also delink the context from its parent, if it has one.
 */
static void
MemoryContextDeleteOnly(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));
	/* We had better not be deleting TopMemoryContext ... */
	Assert(context != TopMemoryContext);
	/* And not CurrentMemoryContext, either */
	Assert(context != CurrentMemoryContext);
	/* All the children should've been deleted already */
	Assert(context->firstchild == NULL);

	/*
	 * It's not entirely clear whether 'tis better to do this before or after
	 * delinking the context; but an error in a callback will likely result in
	 * leaking the whole context (if it's not a root context) if we do it
	 * after, so let's do it before.
	 */
	MemoryContextCallResetCallbacks(context);

	/*
	 * We delink the context from its parent before deleting it, so that if
	 * there's an error we won't have deleted/busted contexts still attached
	 * to the context tree.  Better a leak than a crash.
	 */
	MemoryContextSetParent(context, NULL);

	/*
	 * Also reset the context's ident pointer, in case it points into the
	 * context.  This would only matter if someone tries to get stats on the
	 * (already unlinked) context, which is unlikely, but let's be safe.
	 */
	context->ident = NULL;

	context->methods->delete_context(context);
}

/*
 * MemoryContextDeleteChildren
 *		Delete all the descendants of the named context and release all
 *		space allocated therein.  The named context itself is not touched.
 */
void
MemoryContextDeleteChildren(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/*
	 * MemoryContextDelete will delink the child from me, so just iterate as
	 * long as there is a child.
	 */
	while (context->firstchild != NULL)
		MemoryContextDelete(context->firstchild);
}

/*
 * MemoryContextRegisterResetCallback
 *		Register a function to be called before next context reset/delete.
 *		Such callbacks will be called in reverse order of registration.
 *
 * The caller is responsible for allocating a MemoryContextCallback struct
 * to hold the info about this callback request, and for filling in the
 * "func" and "arg" fields in the struct to show what function to call with
 * what argument.  Typically the callback struct should be allocated within
 * the specified context, since that means it will automatically be freed
 * when no longer needed.
 */
void
MemoryContextRegisterResetCallback(MemoryContext context,
								   MemoryContextCallback *cb)
{
	Assert(MemoryContextIsValid(context));

	/* Push onto head so this will be called before older registrants. */
	cb->next = context->reset_cbs;
	context->reset_cbs = cb;
	/* Mark the context as non-reset (it probably is already). */
	context->isReset = false;
}

/*
 * MemoryContextUnregisterResetCallback
 *		Undo the effects of MemoryContextRegisterResetCallback.
 *
 * This can be used if a callback's effects are no longer required
 * at some point before the context has been reset/deleted.  It is the
 * caller's responsibility to pfree the callback struct (if needed).
 *
 * An assertion failure occurs if the callback was not registered.
 */
void
MemoryContextUnregisterResetCallback(MemoryContext context,
									 MemoryContextCallback *cb)
{
	MemoryContextCallback *prev,
			   *cur;

	Assert(MemoryContextIsValid(context));

	for (prev = NULL, cur = context->reset_cbs; cur != NULL;
		 prev = cur, cur = cur->next)
	{
		if (cur != cb)
			continue;
		if (prev)
			prev->next = cur->next;
		else
			context->reset_cbs = cur->next;
		return;
	}
	Assert(false);
}

/*
 * MemoryContextCallResetCallbacks
 *		Internal function to call all registered callbacks for context.
 */
static void
MemoryContextCallResetCallbacks(MemoryContext context)
{
	MemoryContextCallback *cb;

	/*
	 * We pop each callback from the list before calling.  That way, if an
	 * error occurs inside the callback, we won't try to call it a second time
	 * in the likely event that we reset or delete the context later.
	 */
	while ((cb = context->reset_cbs) != NULL)
	{
		context->reset_cbs = cb->next;
		cb->func(cb->arg);
	}
}

/*
 * MemoryContextSetIdentifier
 *		Set the identifier string for a memory context.
 */
void
MemoryContextSetIdentifier(MemoryContext context, const char *id)
{
	Assert(MemoryContextIsValid(context));
	context->ident = id;
}

/*
 * MemoryContextSetParent
 *		Change a context to belong to a new parent (or no parent).
 *
 * Callers often assume that this function cannot fail, so don't put any
 * elog(ERROR) calls in it.
 */
void
MemoryContextSetParent(MemoryContext context, MemoryContext new_parent)
{
	Assert(MemoryContextIsValid(context));
	Assert(context != new_parent);

	/* Fast path if it's got correct parent already */
	if (new_parent == context->parent)
		return;

	/* Delink from existing parent, if any */
	if (context->parent)
	{
		MemoryContext parent = context->parent;

		if (context->prevchild != NULL)
			context->prevchild->nextchild = context->nextchild;
		else
		{
			Assert(parent->firstchild == context);
			parent->firstchild = context->nextchild;
		}

		if (context->nextchild != NULL)
			context->nextchild->prevchild = context->prevchild;
	}

	/* And relink */
	if (new_parent)
	{
		Assert(MemoryContextIsValid(new_parent));
		context->parent = new_parent;
		context->prevchild = NULL;
		context->nextchild = new_parent->firstchild;
		if (new_parent->firstchild != NULL)
			new_parent->firstchild->prevchild = context;
		new_parent->firstchild = context;
	}
	else
	{
		context->parent = NULL;
		context->prevchild = NULL;
		context->nextchild = NULL;
	}
}

/*
 * MemoryContextAllowInCriticalSection
 *		Allow/disallow allocations in this memory context within a critical
 *		section.
 */
void
MemoryContextAllowInCriticalSection(MemoryContext context, bool allow)
{
	Assert(MemoryContextIsValid(context));

	context->allowInCritSection = allow;
}

/*
 * GetMemoryChunkContext
 *		Given a currently-allocated chunk, determine the MemoryContext that
 *		the chunk belongs to.
 */
MemoryContext
GetMemoryChunkContext(void *pointer)
{
	return MCXT_METHOD(pointer, get_chunk_context) (pointer);
}

/*
 * GetMemoryChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
Size
GetMemoryChunkSpace(void *pointer)
{
	return MCXT_METHOD(pointer, get_chunk_space) (pointer);
}

/*
 * MemoryContextGetParent
 *		Get the parent context (if any) of the specified context
 */
MemoryContext
MemoryContextGetParent(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	return context->parent;
}

/*
 * MemoryContextIsEmpty
 *		Is a memory context empty of any allocated space?
 */
bool
MemoryContextIsEmpty(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	if (context->firstchild != NULL)
		return false;
	return context->methods->is_empty(context);
}

/*
 * Find the memory allocated to blocks for this memory context. If recurse is
 * true, also include children.
 */
Size
MemoryContextMemAllocated(MemoryContext context, bool recurse)
{
	Size		total = context->mem_allocated;

	Assert(MemoryContextIsValid(context));

	if (recurse)
	{
		for (MemoryContext curr = context->firstchild;
			 curr != NULL;
			 curr = MemoryContextTraverseNext(curr, context))
		{
			total += curr->mem_allocated;
		}
	}

	return total;
}

/*
 * Return the memory consumption statistics about the given context and its
 * children.
 */
void
MemoryContextMemConsumed(MemoryContext context,
						 MemoryContextCounters *consumed)
{
	Assert(MemoryContextIsValid(context));

	memset(consumed, 0, sizeof(*consumed));

	/* Examine the context itself */
	context->methods->stats(context, NULL, NULL, consumed, false);

	/* Examine children, using iteration not recursion */
	for (MemoryContext curr = context->firstchild;
		 curr != NULL;
		 curr = MemoryContextTraverseNext(curr, context))
	{
		curr->methods->stats(curr, NULL, NULL, consumed, false);
	}
}

/*
 * MemoryContextCreate
 *		Context-type-independent part of context creation.
 *
 * This is only intended to be called by context-type-specific
 * context creation routines, not by the unwashed masses.
 *
 * node: the as-yet-uninitialized common part of the context header node.
 * tag: NodeTag code identifying the memory context type.
 * method_id: MemoryContextMethodID of the context-type being created.
 * parent: parent context, or NULL if this will be a top-level context.
 * name: name of context (must be statically allocated).
 */
void
MemoryContextCreate(MemoryContext node,
					NodeTag tag,
					MemoryContextMethodID method_id,
					MemoryContext parent,
					const char *name)
{
	/* Validate parent, to help prevent crazy context linkages */
	Assert(parent == NULL || MemoryContextIsValid(parent));
	Assert(node != parent);

	/* Initialize all standard fields of memory context header */
	node->type = tag;
	node->isReset = true;
	node->methods = &mcxt_methods[method_id];
	node->parent = parent;
	node->firstchild = NULL;
	node->mem_allocated = 0;
	node->prevchild = NULL;
	node->name = name;
	node->ident = NULL;
	node->reset_cbs = NULL;

	/* OK to link node into context tree */
	if (parent)
	{
		node->nextchild = parent->firstchild;
		if (parent->firstchild != NULL)
			parent->firstchild->prevchild = node;
		parent->firstchild = node;
		/* inherit allowInCritSection flag from parent */
		node->allowInCritSection = parent->allowInCritSection;
	}
	else
	{
		node->nextchild = NULL;
		node->allowInCritSection = false;
	}
}

/*
 * MemoryContextAllocationFailure
 *		For use by MemoryContextMethods implementations to handle when malloc
 *		returns NULL.  The behavior is specific to whether MCXT_ALLOC_NO_OOM
 *		is in 'flags' (see mctx_internal.h).
 */
void *
MemoryContextAllocationFailure(MemoryContext context, Size size, int flags)
{
	if ((flags & MCXT_ALLOC_NO_OOM) == 0)
		mctx_report_oom(size, context->name);
	return NULL;
}

/*
 * MemoryContextSizeFailure
 *		For use by MemoryContextMethods implementations to handle invalid
 *		memory allocation request sizes.
 */
void
MemoryContextSizeFailure(MemoryContext context, Size size, int flags)
{
	mctx_report_bad_size(size);
}

/*
 * MemoryContextAlloc
 *		Allocate space within the specified context.
 */
void *
MemoryContextAlloc(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	ret = context->methods->alloc(context, size, 0);

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

/*
 * MemoryContextAllocZero
 *		Like MemoryContextAlloc, but clears allocated memory
 */
void *
MemoryContextAllocZero(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	ret = context->methods->alloc(context, size, 0);

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * MemoryContextAllocExtended
 *		Allocate space within the specified context using the given flags.
 */
void *
MemoryContextAllocExtended(MemoryContext context, Size size, int flags)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!((flags & MCXT_ALLOC_HUGE) != 0 ? AllocHugeSizeIsValid(size) :
		  AllocSizeIsValid(size)))
		mctx_report_bad_size(size);

	context->isReset = false;

	ret = context->methods->alloc(context, size, flags);
	if (unlikely(ret == NULL))
		return NULL;

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSetAligned(ret, 0, size);

	return ret;
}

void *
palloc(Size size)
{
	/* duplicates MemoryContextAlloc to avoid increased overhead */
	void	   *ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	ret = context->methods->alloc(context, size, 0);
	/* We expect OOM to be handled by the alloc function */
	Assert(ret != NULL);
	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

void *
palloc0(Size size)
{
	/* duplicates MemoryContextAllocZero to avoid increased overhead */
	void	   *ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	ret = context->methods->alloc(context, size, 0);
	/* We expect OOM to be handled by the alloc function */
	Assert(ret != NULL);
	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetAligned(ret, 0, size);

	return ret;
}

void *
palloc_extended(Size size, int flags)
{
	/* duplicates MemoryContextAllocExtended to avoid increased overhead */
	void	   *ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	ret = context->methods->alloc(context, size, flags);
	if (unlikely(ret == NULL))
	{
		/* NULL can be returned only when using MCXT_ALLOC_NO_OOM */
		Assert(flags & MCXT_ALLOC_NO_OOM);
		return NULL;
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * MemoryContextAllocAligned
 *		Allocate 'size' bytes of memory in 'context' aligned to 'alignto'
 *		bytes.
 *
 * 'alignto' must be a power of 2.
 * 'flags' may be 0 or set the same as MemoryContextAllocExtended().
 */
void *
MemoryContextAllocAligned(MemoryContext context,
						  Size size, Size alignto, int flags)
{
	MemoryChunk *alignedchunk;
	Size		alloc_size;
	void	   *unaligned;
	void	   *aligned;

	Assert(alignto < (128 * 1024 * 1024));
	/* ensure alignto is a power of 2 */
	Assert((alignto & (alignto - 1)) == 0);

	/*
	 * If the alignment requirements are less than what we already guarantee
	 * then just use the standard allocation function.
	 */
	if (unlikely(alignto <= MAXIMUM_ALIGNOF))
		return MemoryContextAllocExtended(context, size, flags);

	/*
	 * We implement aligned pointers by simply allocating enough memory for
	 * the requested size plus the alignment and an additional "redirection"
	 * MemoryChunk. We use this redirection MemoryChunk in order to find the
	 * pointer to the memory that was returned by the MemoryContextAllocExtended
	 * call below, by "borrowing" the block offset field to point back to the
	 * original allocated address instead of an owning block.
	 */
	alloc_size = size + PallocAlignedExtraBytes(alignto);

#ifdef MEMORY_CONTEXT_CHECKING
	/* ensure there's space for a sentinel byte */
	alloc_size += 1;
#endif

	/*
	 * Perform the actual allocation, but do not pass down MCXT_ALLOC_ZERO.
	 * This ensures that wasted bytes beyond the aligned chunk do not become
	 * DEFINED.
	 */
	unaligned = MemoryContextAllocExtended(context, alloc_size,
										   flags & ~MCXT_ALLOC_ZERO);

	if (unlikely(unaligned == NULL))
	{
		/* NULL can be returned only when using MCXT_ALLOC_NO_OOM */
		Assert(flags & MCXT_ALLOC_NO_OOM);
		return NULL;
	}

	/* compute the aligned pointer */
	aligned = (void *) TYPEALIGN(alignto, (char *) unaligned +
								 sizeof(MemoryChunk));

	alignedchunk = PointerGetMemoryChunk(aligned);

	/*
	 * We set the redirect MemoryChunk so that the block offset calculation is
	 * used to point back to the 'unaligned' allocated chunk, and store
	 * 'alignto' in the 'value' field so we know it if asked to realloc.
	 */
	MemoryChunkSetHdrMask(alignedchunk, unaligned, alignto,
						  MCTX_ALIGNED_REDIRECT_ID);

	/* double check we produced a correctly aligned pointer */
	Assert((void *) TYPEALIGN(alignto, aligned) == aligned);

#ifdef MEMORY_CONTEXT_CHECKING
	alignedchunk->requested_size = size;
	/* set mark to catch clobber of "unused" space */
	set_sentinel(aligned, size);
#endif

	VALGRIND_MEMPOOL_FREE(context, unaligned);
	VALGRIND_MEMPOOL_ALLOC(context, aligned, size);

	/* Now zero (and make DEFINED) just the aligned chunk, if requested */
	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSetAligned(aligned, 0, size);

	return aligned;
}

/*
 * palloc_aligned
 *		Allocate 'size' bytes returning a pointer that's aligned to the
 *		'alignto' boundary.
 */
void *
palloc_aligned(Size size, Size alignto, int flags)
{
	return MemoryContextAllocAligned(CurrentMemoryContext, size, alignto, flags);
}

/*
 * pfree
 *		Release an allocated chunk.
 */
void
pfree(void *pointer)
{
#ifdef USE_VALGRIND
	MemoryContext context = GetMemoryChunkContext(pointer);
#endif

	MCXT_METHOD(pointer, free_p) (pointer);

	VALGRIND_MEMPOOL_FREE(context, pointer);
}

/*
 * repalloc
 *		Adjust the size of a previously allocated chunk.
 */
void *
repalloc(void *pointer, Size size)
{
#if defined(USE_VALGRIND)
	MemoryContext context = GetMemoryChunkContext(pointer);
#endif
	void	   *ret;

	AssertNotInCriticalSection(context);

	ret = MCXT_METHOD(pointer, realloc) (pointer, size, 0);

	VALGRIND_MEMPOOL_CHANGE(context, pointer, ret, size);

	return ret;
}

/*
 * repalloc_extended
 *		Adjust the size of a previously allocated chunk,
 *		with HUGE and NO_OOM options.
 */
void *
repalloc_extended(void *pointer, Size size, int flags)
{
#if defined(USE_VALGRIND)
	MemoryContext context = GetMemoryChunkContext(pointer);
#endif
	void	   *ret;

	AssertNotInCriticalSection(context);

	ret = MCXT_METHOD(pointer, realloc) (pointer, size, flags);
	if (unlikely(ret == NULL))
		return NULL;

	VALGRIND_MEMPOOL_CHANGE(context, pointer, ret, size);

	return ret;
}

/*
 * repalloc0
 *		Adjust the size of a previously allocated chunk and zero out the added
 *		space.
 */
void *
repalloc0(void *pointer, Size oldsize, Size size)
{
	void	   *ret;

	/* catch wrong argument order */
	if (unlikely(oldsize > size))
		mctx_report_bad_size(size);

	ret = repalloc(pointer, size);
	memset((char *) ret + oldsize, 0, (size - oldsize));
	return ret;
}

/*
 * Support for safe calculation of memory request sizes
 */
Size
add_size(Size s1, Size s2)
{
	Size		result;

	if (unlikely(pg_add_size_overflow(s1, s2, &result)))
		mctx_report_size_overflow();
	return result;
}

Size
mul_size(Size s1, Size s2)
{
	Size		result;

	if (unlikely(pg_mul_size_overflow(s1, s2, &result)))
		mctx_report_size_overflow();
	return result;
}

/*
 * palloc_mul
 *		Equivalent to palloc(mul_size(s1, s2)).
 */
void *
palloc_mul(Size s1, Size s2)
{
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req)))
		mctx_report_size_overflow();
	return palloc(req);
}

/*
 * palloc0_mul
 *		Equivalent to palloc0(mul_size(s1, s2)).
 */
void *
palloc0_mul(Size s1, Size s2)
{
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req)))
		mctx_report_size_overflow();
	return palloc0(req);
}

/*
 * palloc_mul_extended
 *		Equivalent to palloc_extended(mul_size(s1, s2), flags).
 */
void *
palloc_mul_extended(Size s1, Size s2, int flags)
{
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req)))
		mctx_report_size_overflow();
	return palloc_extended(req, flags);
}

/*
 * repalloc_mul
 *		Equivalent to repalloc(p, mul_size(s1, s2)).
 */
void *
repalloc_mul(void *p, Size s1, Size s2)
{
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req)))
		mctx_report_size_overflow();
	return repalloc(p, req);
}

/*
 * repalloc_mul_extended
 *		Equivalent to repalloc_extended(p, mul_size(s1, s2), flags).
 */
void *
repalloc_mul_extended(void *p, Size s1, Size s2, int flags)
{
	Size		req;

	if (unlikely(pg_mul_size_overflow(s1, s2, &req)))
		mctx_report_size_overflow();
	return repalloc_extended(p, req, flags);
}

/*
 * MemoryContextAllocHuge
 *		Allocate (possibly-expansive) space within the specified context.
 */
void *
MemoryContextAllocHuge(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	context->isReset = false;

	ret = context->methods->alloc(context, size, MCXT_ALLOC_HUGE);

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

/*
 * repalloc_huge
 *		Adjust the size of a previously allocated chunk, permitting a large
 *		value.  The previous allocation need not have been "huge".
 */
void *
repalloc_huge(void *pointer, Size size)
{
	return repalloc_extended(pointer, size, MCXT_ALLOC_HUGE);
}

/*
 * MemoryContextStrdup
 *		Like strdup(), but allocate from the specified context
 */
char *
MemoryContextStrdup(MemoryContext context, const char *string)
{
	char	   *nstr;
	Size		len = strlen(string) + 1;

	nstr = (char *) MemoryContextAlloc(context, len);

	memcpy(nstr, string, len);

	return nstr;
}

char *
pstrdup(const char *in)
{
	return MemoryContextStrdup(CurrentMemoryContext, in);
}

/*
 * pnstrdup
 *		Like pstrdup(), but append null byte to a
 *		not-necessarily-null-terminated input string.
 */
char *
pnstrdup(const char *in, Size len)
{
	char	   *out;

	len = strnlen(in, len);

	out = palloc(len + 1);
	memcpy(out, in, len);
	out[len] = '\0';

	return out;
}

/*
 * Make copy of string with all trailing newline characters removed.
 */
char *
pchomp(const char *in)
{
	size_t		n;

	n = strlen(in);
	while (n > 0 && in[n - 1] == '\n')
		n--;
	return pnstrdup(in, n);
}
