/*-------------------------------------------------------------------------
 *
 * receivelog.c - receive transaction log files using the streaming
 *				  replication protocol.
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
	int			f;
	char		fn[MAXPGPATH];

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
bool
ReceiveXlogStream(PGconn *conn, XLogRecPtr startpos, uint32 timeline, char *basedir, segment_finish_callback segment_finish)
{
	char		query[128];
	char		current_walfile_name[MAXPGPATH];
	PGresult   *res;
	char	   *copybuf = NULL;
	int			walfile = -1;

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
		int			r;
		int			xlogoff;
		int			bytes_left;
		int			bytes_written;

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

		/*
		 * Verify that the initial location in the stream matches where
		 * we think we are.
		 */
		if (walfile == -1)
		{
			/* No file open yet */
			if (xlogoff != 0)
			{
				fprintf(stderr, _("%s: received xlog record for offset %u with no file open\n"),
						progname, xlogoff);
				return false;
			}
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
		}

		bytes_left = r - STREAMING_HEADER_SIZE;
		bytes_written = 0;

		while (bytes_left)
		{
			int bytes_to_write;

			/*
			 * If crossing a WAL boundary, only write up until we reach
			 * XLOG_SEG_SIZE.
			 */
			if (xlogoff + bytes_left > XLOG_SEG_SIZE)
				bytes_to_write = XLOG_SEG_SIZE - xlogoff;
			else
				bytes_to_write = bytes_left;

			if (walfile == -1)
			{
				walfile = open_walfile(blockstart, timeline,
									   basedir, current_walfile_name);
				if (walfile == -1)
					/* Error logged by open_walfile */
					return false;
			}

			if (write(walfile,
					  copybuf + STREAMING_HEADER_SIZE + bytes_written,
					  bytes_to_write) != bytes_to_write)
			{
				fprintf(stderr, _("%s: could not write %u bytes to WAL file %s: %s\n"),
						progname,
						bytes_to_write,
						current_walfile_name,
						strerror(errno));
				return false;
			}

			/* Write was successful, advance our position */
			bytes_written += bytes_to_write;
			bytes_left -= bytes_to_write;
			blockstart.xrecoff += bytes_to_write;
			xlogoff += bytes_to_write;

			/* Did we reach the end of a WAL segment? */
			if (blockstart.xrecoff % XLOG_SEG_SIZE == 0)
			{
				fsync(walfile);
				close(walfile);
				walfile = -1;
				xlogoff = 0;

				if (segment_finish != NULL)
				{
					/*
					 * Callback when the segment finished, and return if it told
					 * us to.
					 */
					if (segment_finish(blockstart, timeline))
						return true;
				}
			}
		}
		/* No more data left to send, get the next copy packet */
	}

	/*
	 * The only way to get out of the loop is if the server shut down the
	 * replication stream. If it's a controlled shutdown, the server will send
	 * a shutdown message, and we'll return the latest xlog location that has
	 * been streamed.
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
