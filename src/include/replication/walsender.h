/*-------------------------------------------------------------------------
 *
 * walsender.h
 *	  Exports from replication/walsender.c.
 *
 * Portions Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * src/include/replication/walsender.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALSENDER_H
#define _WALSENDER_H

#include "access/xlog.h"
#include "nodes/nodes.h"
#include "storage/latch.h"
#include "storage/spin.h"

/*
 * Each walsender has a WalSnd struct in shared memory.
 */
typedef struct WalSnd
{
	pid_t		pid;			/* this walsender's process id, or 0 */
	XLogRecPtr	sentPtr;		/* WAL has been sent up to this point */

	slock_t		mutex;			/* locks shared variables shown above */

	/*
	 * Latch used by backends to wake up this walsender when it has work
	 * to do.
	 */
	Latch		latch;
} WalSnd;

/* There is one WalSndCtl struct for the whole database cluster */
typedef struct
{
	WalSnd		walsnds[1];		/* VARIABLE LENGTH ARRAY */
} WalSndCtlData;

extern WalSndCtlData *WalSndCtl;

/* global state */
extern bool am_walsender;

/* user-settable parameters */
extern int	WalSndDelay;
extern int	max_wal_senders;

extern int	WalSenderMain(void);
extern void WalSndSignals(void);
extern Size WalSndShmemSize(void);
extern void WalSndShmemInit(void);
extern void WalSndWakeup(void);

extern Datum pg_stat_get_wal_senders(PG_FUNCTION_ARGS);

/*
 * Internal functions for parsing the replication grammar, in repl_gram.y and
 * repl_scanner.l
 */
extern int	replication_yyparse(void);
extern int	replication_yylex(void);
extern void replication_yyerror(const char *str);
extern void replication_scanner_init(const char *query_string);
extern void replication_scanner_finish(void);

extern Node *replication_parse_result;

#endif   /* _WALSENDER_H */
