/*-------------------------------------------------------------------------
 *
 * pg_receivexlog.c - receive streaming transaction log data and write it
 *					  to a local file.
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_receivexlog.c
 *-------------------------------------------------------------------------
 */


#include "postgres_fe.h"

#include "libpq-fe.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "getopt_long.h"

#include "receivelog.h"
#include "streamutil.h"


/* Global options */
char	   *basedir = NULL;
int			verbose = 0;


static void usage(void);
static XLogRecPtr FindStreamingStart(XLogRecPtr currentpos, uint32 currenttimeline);
static void StreamLog();
static bool segment_callback(XLogRecPtr segendpos, uint32 timeline);

/*
 * XXX: from xlog_internal.h
 */
#define XLogSegsPerFile (((uint32) 0xffffffff) / XLOG_SEG_SIZE)
#define PrevLogSeg(logId, logSeg)       \
        do { \
                if (logSeg) \
                        (logSeg)--; \
                else \
                { \
                        (logId)--; \
                        (logSeg) = XLogSegsPerFile-1; \
                } \
        } while (0)


static void
usage(void)
{
	printf(_("%s receives PostgreSQL streaming transaction logs\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions controlling the output:\n"));
	printf(_("  -D, --dir=directory       receive xlog files into this directory\n"));
	printf(_("\nGeneral options:\n"));
	printf(_("  -v, --verbose             output verbose messages\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -w, --no-password        never prompt for password\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}

static bool
segment_callback(XLogRecPtr segendpos, uint32 timeline)
{
	char fn[MAXPGPATH];
	struct stat statbuf;

	if (verbose)
		fprintf(stderr, _("%s: finished segment at %X/%X (timeline %u)\n"),
				progname, segendpos.xlogid, segendpos.xrecoff, timeline);

	/*
	 * Check if there is a partial file for the name we just finished,
	 * and if there is, remove it under the assumption that we have now
	 * got all the data we need.
	 */
	PrevLogSeg(segendpos.xlogid, segendpos.xrecoff);
	snprintf(fn, sizeof(fn), "%s/%08X%08X%08X.partial",
			 basedir, timeline,
			 segendpos.xlogid,
			 segendpos.xrecoff / XLOG_SEG_SIZE);
	if (stat(fn, &statbuf) == 0)
	{
		/* File existed, get rid of it */
		if (verbose)
			fprintf(stderr, _("%s: removing file \"%s\"\n"),
					progname, fn);
		unlink(fn);
	}

	/* Never abort */
	return false;
}

/*
 * Determine starting location for streaming, based on:
 * 1. If there are existing xlog segments, start at the end of the last one
 * 2. If the last one is a partial segment, rename it and start over, since
 *    we don't sync after every write.
 * 3. If no existing xlog exists, start from the beginning of the current
 *    WAL segment.
 */
static XLogRecPtr
FindStreamingStart(XLogRecPtr currentpos, uint32 currenttimeline)
{
	DIR	   *dir;
	struct dirent *dirent;
	int		i;
	bool	b;
	XLogRecPtr high = {0,0};

	dir = opendir(basedir);
	if (dir == NULL)
	{
		fprintf(stderr, _("%s: could not open directory \"%s\": %s\n"),
				progname, basedir, strerror(errno));
		disconnect_and_exit(1);
	}

	while ((dirent = readdir(dir)) != NULL)
	{
		char fullpath[MAXPGPATH];
		struct stat statbuf;
		uint32	tli, log, seg;

		if (!strcmp(dirent->d_name, ".") || !strcmp(dirent->d_name, ".."))
			continue;

		/* xlog files are always 24 characters */
		if (strlen(dirent->d_name) != 24)
			continue;

		/* Filenames are always made out of 0-9 and A-F */
		b = false;
		for (i = 0; i < 24; i++)
		{
			if (!(dirent->d_name[i] >= '0' && dirent->d_name[i] <= '9') &&
				!(dirent->d_name[i] >= 'A' && dirent->d_name[i] <= 'F'))
			{
				b = true;
				break;
			}
		}
		if (b)
			continue;

		/*
		 * Looks like an xlog file. Parse it's position.
		 */
		if (sscanf(dirent->d_name, "%08X%08X%08X", &tli, &log, &seg) != 3)
		{
			fprintf(stderr, _("%s: could not parse xlog filename \"%s\"\n"),
					progname, dirent->d_name);
			disconnect_and_exit(1);
		}
		log *= XLOG_SEG_SIZE;

		/* Ignore any files that are for another timeline */
		if (tli != currenttimeline)
			continue;

		/* Check if this is a completed segment or not */
		snprintf(fullpath, sizeof(fullpath),  "%s/%s", basedir, dirent->d_name);
		if (stat(fullpath, &statbuf) != 0)
		{
			fprintf(stderr, _("%s: could not stat file \"%s\": %s\n"),
					progname, fullpath, strerror(errno));
			disconnect_and_exit(1);
		}

		if (statbuf.st_size == 16 * 1024 * 1024)
		{
			/* Completed segment */
			if (log > high.xlogid ||
				(log == high.xlogid && seg > high.xrecoff))
			{
				high.xlogid = log;
				high.xrecoff = seg;
				continue;
			}
		}
		else
		{
			/*
			 * This is a partial file. Rename it out of the way.
			 */
			char newfn[MAXPGPATH];

			fprintf(stderr, _("%s: renaming partial file \"%s\" to \"%s.partial\"\n"),
					progname, dirent->d_name, dirent->d_name);

			snprintf(newfn, sizeof(newfn), "%s/%s.partial",
					 basedir, dirent->d_name);

			if (stat(newfn, &statbuf) == 0)
			{
				fprintf(stderr, _("%s: file \"%s\" already exists. Check and clean up manually.\n"),
						progname, newfn);
				disconnect_and_exit(1);
			}
			if (rename(fullpath, newfn) != 0)
			{
				fprintf(stderr, _("%s: could not rename \"%s\" to \"%s\": %s\n"),
						progname, fullpath, newfn, strerror(errno));
				disconnect_and_exit(1);
			}

			/* Don't continue looking for more, we assume this is the last */
			break;
		}
	}

	closedir(dir);

	if (high.xlogid > 0 && high.xrecoff > 0)
		return high;
	return currentpos;
}

/*
 * Start the log streaming
 */
static void
StreamLog(void)
{
	PGresult   *res;
	uint32		timeline;
	XLogRecPtr	startpos;

	/*
	 * Connect in replication mode to the server
	 */
	conn = GetConnection();

	/*
	 * Run IDENFITY_SYSTEM so we can get the timeline
	 */
	res = PQexec(conn, "IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: could not identify system: %s\n"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}
	if (PQntuples(res) != 1)
	{
		fprintf(stderr, _("%s: could not identify system, got %i rows\n"),
				progname, PQntuples(res));
		disconnect_and_exit(1);
	}
	timeline = atoi(PQgetvalue(res, 0, 1));
	if (sscanf(PQgetvalue(res, 0, 2), "%X/%X", &startpos.xlogid, &startpos.xrecoff) != 2)
	{
		fprintf(stderr, _("%s: could not parse log start position from value \"%s\"\n"),
				progname, PQgetvalue(res, 0, 2));
		disconnect_and_exit(1);
	}
	PQclear(res);

	/*
	 * Figure out where to start streaming.
	 */
	startpos = FindStreamingStart(startpos, timeline);

	/*
	 * Always start streaming at the beginning of a segment
	 */
	startpos.xrecoff -= startpos.xrecoff % XLOG_SEG_SIZE;

	/*
	 * Start the replication
	 */
	if (verbose)
		fprintf(stderr, _("%s: starting log streaming at %X/%X (timeline %u)\n"),
				progname, startpos.xlogid, startpos.xrecoff, timeline);

	ReceiveXlogStream(conn, startpos, timeline, basedir, segment_callback);
}

int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"dir", required_argument, NULL, 'D'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};
	int			c;

	int			option_index;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_receivexlog"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0
				 || strcmp(argv[1], "--version") == 0)
		{
			puts("pg_receivexlog (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "D:h:p:U:wWv",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
				basedir = xstrdup(optarg);
				break;
			case 'h':
				dbhost = xstrdup(optarg);
				break;
			case 'p':
				if (atoi(optarg) <= 0)
				{
					fprintf(stderr, _("%s: invalid port number \"%s\"\n"),
							progname, optarg);
					exit(1);
				}
				dbport = xstrdup(optarg);
				break;
			case 'U':
				dbuser = xstrdup(optarg);
				break;
			case 'w':
				dbgetpassword = -1;
				break;
			case 'W':
				dbgetpassword = 1;
				break;
			case 'v':
				verbose++;
				break;
			default:

				/*
				 * getopt_long already emitted a complaint
				 */
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
		}
	}

	/*
	 * Any non-option arguments?
	 */
	if (optind < argc)
	{
		fprintf(stderr,
				_("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Required arguments
	 */
	if (basedir == NULL)
	{
		fprintf(stderr, _("%s: no target directory specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	StreamLog();

	exit(0);
}
