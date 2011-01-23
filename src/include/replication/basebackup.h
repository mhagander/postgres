/*-------------------------------------------------------------------------
 *
 * basebackup.h
 *	  Exports from replication/basebackup.c.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 * src/include/replication/walsender.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _BASEBACKUP_H
#define _BASEBACKUP_H

typedef struct
{
	const char *label;
	bool		progress;
	bool		fastcheckpoint;
}	basebackup_options;

extern void SendBaseBackup(basebackup_options *opt);

#endif   /* _BASEBACKUP_H */
