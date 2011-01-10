/*-------------------------------------------------------------------------
 *
 * replnodes.h
 *	  definitions for replication grammar parse nodes
 *
 * Many of the node types used in parsetrees include a "location" field.
 * This is a byte (not character) offset in the original source text, to be
 * used for positioning an error cursor when there is an error related to
 * the node.  Access to the original source text is needed to make use of
 * the location.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/replnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPLNODES_H
#define REPLNODES_H

#include "access/xlogdefs.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"

/* ----------------------
 *		IDENTIFY_SYSTEM command
 * ----------------------
 */
typedef struct IdentifySystemCmd
{
	NodeTag		type;
} IdentifySystemCmd;


/* ----------------------
 *		BASE_BACKUP command
 * ----------------------
 */
typedef struct BaseBackupCmd
{
	NodeTag		type;
	char	   *label;
	bool		progress;
} BaseBackupCmd;


/* ----------------------
 *		START_REPLICATION command
 * ----------------------
 */
typedef struct StartReplicationCmd
{
	NodeTag		type;
	XLogRecPtr	startpoint;
} StartReplicationCmd;

#endif   /* REPLNODES_H */
