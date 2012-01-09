/*-------------------------------------------------------------------------
 *
 * spi_priv.h
 *				Server Programming Interface private declarations
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/spi_priv.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPI_PRIV_H
#define SPI_PRIV_H

#include "executor/spi.h"


#define _SPI_PLAN_MAGIC		569278163

typedef struct
{
	/* current results */
	uint32		processed;		/* by Executor */
	Oid			lastoid;
	SPITupleTable *tuptable;

	MemoryContext procCxt;		/* procedure context */
	MemoryContext execCxt;		/* executor context */
	MemoryContext savedcxt;		/* context of SPI_connect's caller */
	SubTransactionId connectSubid;		/* ID of connecting subtransaction */
} _SPI_connection;

/*
 * SPI plans have three states: saved, unsaved, or temporary.
 *
 * Ordinarily, the _SPI_plan struct itself as well as the argtypes array
 * are in a dedicated memory context identified by plancxt (which can be
 * really small).  All the other subsidiary state is in plancache entries
 * identified by plancache_list (note: the list cells themselves are in
 * plancxt).
 *
 * In an unsaved plan, the plancxt as well as the plancache entries' contexts
 * are children of the SPI procedure context, so they'll all disappear at
 * function exit.  plancache.c also knows that the plancache entries are
 * "unsaved", so it doesn't link them into its global list; hence they do
 * not respond to inval events.  This is OK since we are presumably holding
 * adequate locks to prevent other backends from messing with the tables.
 *
 * For a saved plan, the plancxt is made a child of CacheMemoryContext
 * since it should persist until explicitly destroyed.  Likewise, the
 * plancache entries will be under CacheMemoryContext since we tell
 * plancache.c to save them.  We rely on plancache.c to keep the cache
 * entries up-to-date as needed in the face of invalidation events.
 *
 * There are also "temporary" SPI plans, in which the _SPI_plan struct is
 * not even palloc'd but just exists in some function's local variable.
 * The plancache entries are unsaved and exist under the SPI executor context,
 * while additional data such as argtypes and list cells is loose in the SPI
 * executor context.  Such plans can be identified by having plancxt == NULL.
 *
 * Note: if the original query string contained only whitespace and comments,
 * the plancache_list will be NIL and so there is no place to store the
 * query string.  We don't care about that, but we do care about the
 * argument type array, which is why it's seemingly-redundantly stored.
 */
typedef struct _SPI_plan
{
	int			magic;			/* should equal _SPI_PLAN_MAGIC */
	bool		saved;			/* saved or unsaved plan? */
	List	   *plancache_list; /* one CachedPlanSource per parsetree */
	MemoryContext plancxt;		/* Context containing _SPI_plan and data */
	int			cursor_options; /* Cursor options used for planning */
	int			nargs;			/* number of plan arguments */
	Oid		   *argtypes;		/* Argument types (NULL if nargs is 0) */
	ParserSetupHook parserSetup;	/* alternative parameter spec method */
	void	   *parserSetupArg;
} _SPI_plan;

#endif   /* SPI_PRIV_H */
