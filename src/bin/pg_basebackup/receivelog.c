/*-------------------------------------------------------------------------
 *
 * receivelog.c - receive transaction log files using the streaming
 *                replication protocol.
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/receivelog.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "libpq-fe.h"

#include <sys/types.h>
#include <unistd.h>

#include "receivelog.h"
#include "streamutil.h"

/* XXX: from xlog_internal.h */
#define MAXFNAMELEN		64
#define XLogFileName(fname, tli, log, seg)	\
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X", tli, log, seg)

/* Size of the streaming replication protocol header */
#define STREAMING_HEADER_SIZE (1+8+8+8)

/*
 * Open a new WAL file in the specified directory. Store the name
 * (not including the full directory) in namebuf. Assumes there is
 * enough room in this buffer...
 */
static int
open_walfile(XLogRecPtr startpoint, uint32 timeline, char *basedir, char *namebuf)
{
	int f;
	char fn[MAXPGPATH];

	XLogFileName(namebuf, timeline, startpoint.xlogid,
				 startpoint.xrecoff / XLOG_SEG_SIZE);

	snprintf(fn, sizeof(fn), "%s/%s", basedir, namebuf);
	f = open(fn, O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (f == -1)
		fprintf(stderr, _("%s: Could not open WAL segment %s: %s\n"),
				progname, namebuf, strerror(errno));
	return f;
}

/*
 * Receive a log stream starting at the specified position.
 *
 * Note: The log position *must* be at a log segment change, or we will
 * end up streaming an incomplete file.
 */
bool ReceiveXlogStream(PGconn *conn, XLogRecPtr startpos, uint32 timeline, char *basedir, segment_finish_callback segment_finish)
{
	char query[128];
	char current_walfile_name[MAXPGPATH];
	PGresult *res;
	char *copybuf = NULL;
	int walfile = -1;

	/* Initiate the replication stream at specified location */
	snprintf(query, sizeof(query), "START_REPLICATION %X/%X", startpos.xlogid, startpos.xrecoff);
	res = PQexec(conn, query);
	if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		fprintf(stderr, _("%s: could not start replication: %s\n"),
				progname, PQresultErrorMessage(res));
		return false;
	}
	PQclear(res);

	/*
	 * Receive the actual xlog data
	 */
	while (1)
	{
		XLogRecPtr	blockstart;
		int r;
		int xlogoff;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		r = PQgetCopyData(conn, &copybuf, 0);
		if (r == -1)
			/* End of copy stream */
			break;
		if (r == -2)
		{
			fprintf(stderr, _("%s: could not read copy data: %s\n"),
					progname, PQerrorMessage(conn));
			return false;
		}
		if (r < STREAMING_HEADER_SIZE + 1)
		{
			fprintf(stderr, _("%s: streaming header too small: %i\n"),
					progname, r);
			return false;
		}
		if (copybuf[0] != 'w')
		{
			fprintf(stderr, _("%s: streaming header corrupt: \"%c\"\n"),
					progname, copybuf[0]);
			return false;
		}

		/* Extract WAL location for this block */
		memcpy(&blockstart, copybuf + 1, 8);

		xlogoff = blockstart.xrecoff % XLOG_SEG_SIZE;

		if (walfile == -1)
		{
			/* No file open yet */
			if (xlogoff != 0)
			{
				fprintf(stderr, _("%s: received xlog record for offset %u with no file open\n"),
						progname, xlogoff);
				return false;
			}
			walfile = open_walfile(blockstart, timeline,
								   basedir, current_walfile_name);
			if (walfile == -1)
				return false;
		}
		else
		{
			/* More data in existing segment */
			/* XXX: store seek value don't reseek all the time */
			if (lseek(walfile, 0, SEEK_CUR) != xlogoff)
			{
				fprintf(stderr, _("%s: got WAL data offset %i, expected %i\n"),
						progname, xlogoff, (int) lseek(walfile, 0, SEEK_CUR));
				return false;
			}
			/* Position matches, write happens lower down */
		}

		/* We have a file open in the correct position */
		if (write(walfile, copybuf + STREAMING_HEADER_SIZE,
				  r - STREAMING_HEADER_SIZE) != r - STREAMING_HEADER_SIZE)
		{
			fprintf(stderr, _("%s: could not write %u bytes to WAL file %s: %s\n"),
					progname,
					r - STREAMING_HEADER_SIZE,
					current_walfile_name,
					strerror(errno));
			return false;
		}

		/* XXX: callback after each write */

		/* Check if we are at the end of a segment */
		if (lseek(walfile, 0, SEEK_CUR) == XLOG_SEG_SIZE)
		{
			/* Offset zero in new file, close and sync the old one */
			fsync(walfile);
			close(walfile);
			walfile = -1;

			if (segment_finish != NULL)
			{
				/*
				 * Callback when the segment finished, and return if it told
				 * us to.
				 *
				 * A block in the wal stream can never cross a segment
				 * boundary, so we can safely just add the current block size
				 * to the offset, so the xlog pointer points to what we have
				 * actually written.
				 */
				blockstart.xrecoff += r - STREAMING_HEADER_SIZE;
				if (segment_finish(blockstart, timeline))
					return true;
			}
		}
	}

	/*
	 * The only way to get out of the loop is if the server shut down the
	 * replication stream. If it's a controlled shutdown, the server will
	 * send a shutdown message, and we'll return the latest xlog location
	 * that has been streamed.
	 */

	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: unexpected termination of replication stream: %s\n"),
				progname, PQresultErrorMessage(res));
		return false;
	}
	PQclear(res);
	return true;
}
