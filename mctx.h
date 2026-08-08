#ifndef MCTX_H
#define MCTX_H

/*
 * Single include for consumers of this library. Pulls in the full public
 * API: context lifecycle and accounting (memutils.h) and the palloc/pfree
 * allocation family (palloc.h). Implementation files (mcxt.c, aset.c,
 * alignedalloc.c) include the specific headers they need directly instead
 * of this one.
 */

#include "memutils.h"
#include "palloc.h"

#endif							/* MCTX_H */
