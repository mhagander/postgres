
/*-------------------------------------------------------------------------
 *
 * pg_basebackup.c - receive a base backup using streaming replication protocol
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "libpq-fe.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "getopt_long.h"


/* Global options */
static const char *progname;
char	   *basedir = NULL;
char	   *tardir = NULL;
char	   *label = "pg_basebackup base backup";
bool		showprogress = false;
int			verbose = 0;
char	   *conninfo = NULL;

/* Progress counters */
static uint64 totalsize;
static uint64 totaldone;
static int	tablespacecount;

/* Function headers */
static char *xstrdup(const char *s);
static void usage(void);
static void verify_dir_is_empty_or_create(char *dirname);
static PGconn *GetConnection(void);

static void ReceiveTarFile(PGconn *conn, PGresult *res, int rownum);
static void ReceiveAndUnpackTarFile(PGconn *conn, PGresult *res, int rownum);
static void BaseBackup();


static char *
xstrdup(const char *s)
{
	char	   *result;

	result = strdup(s);
	if (!result)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		exit(1);
	}
	return result;
}


static void
usage(void)
{
	printf(_("%s takes base backups of running PostgreSQL servers\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -c, --conninfo=conninfo   connection info string to server\n"));
	printf(_("  -d, --basedir=directory   receive base backup into directory\n"));
	printf(_("  -t, --tardir=directory    receive base backup into tar files\n"
			 "                            stored in specified directory\n"));
	printf(_("  -l, --label=label         set backup label\n"));
	printf(_("  -p, --progress            show progress information\n"));
	printf(_("  -v, --verbose             output verbose messages\n"));
	printf(_("\nOther options:\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
}

static void
verify_dir_is_empty_or_create(char *dirname)
{
	switch (pg_check_dir(dirname))
	{
		case 0:

			/*
			 * Does not exist, so create
			 */
			if (mkdir(dirname, S_IRWXU) == -1)
			{
				fprintf(stderr,
						_("%s: could not create directory \"%s\": %s\n"),
						progname, dirname, strerror(errno));
				exit(1);
			}
			return;
		case 1:

			/*
			 * Exists, empty
			 */
			return;
		case 2:

			/*
			 * Exists, not empty
			 */
			fprintf(stderr,
					_("%s: directory \"%s\" exists but is not empty\n"),
					progname, dirname);
			exit(1);
		case -1:

			/*
			 * Access problem
			 */
			fprintf(stderr, _("%s: could not access directory \"%s\": %s\n"),
					progname, dirname, strerror(errno));
			exit(1);
	}
}

static void
ReceiveTarFile(PGconn *conn, PGresult *res, int rownum)
{
	char		fn[MAXPGPATH];
	char	   *copybuf = NULL;
	FILE	   *tarfile = NULL;

	if (PQgetisnull(res, rownum, 0))

		/*
		 * Base tablespaces
		 */
		if (strcmp(tardir, "-") == 0)
			tarfile = stdout;
		else
		{
			sprintf(fn, "%s/base.tar", tardir);
			tarfile = fopen(fn, "wb");
		}
	else
	{

		/*
		 * Specific tablespace
		 */
		sprintf(fn, "%s/%s.tar", tardir, PQgetvalue(res, rownum, 0));
		tarfile = fopen(fn, "wb");
	}

	if (!tarfile)
	{
		fprintf(stderr, _("%s: could not create file \"%s\": %s\n"),
				progname, fn, strerror(errno));
		exit(1);
	}

	/*
	 * Get the COPY data stream
	 */
	res = PQgetResult(conn);
	if (!res || PQresultStatus(res) != PGRES_COPY_OUT)
	{
		fprintf(stderr, _("%s: could not get COPY data stream: %s\n"),
				progname, PQerrorMessage(conn));
		exit(1);
	}

	while (1)
	{
		int			r;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		r = PQgetCopyData(conn, &copybuf, 0);
		if (r == -1)
		{
			/*
			 * End of chunk. Close file (but not stdout).
			 *
			 * Also, write two completely empty blocks at the end
			 * of the tar file, as required by some tar programs.
			 */
			char zerobuf[1024];

			MemSet(zerobuf, 0, sizeof(zerobuf));
			fwrite(zerobuf, sizeof(zerobuf), 1, tarfile);

			if (strcmp(tardir, "-") != 0)
				fclose(tarfile);

			break;
		}
		else if (r == -2)
		{
			fprintf(stderr, _("%s: could not read COPY data: %s\n"),
					progname, PQerrorMessage(conn));
			exit(1);
		}

		fwrite(copybuf, r, 1, tarfile);
		if (showprogress)
		{
			totaldone += r;
			if (verbose)
				fprintf(stderr,
						"%llu/%llu kB (%i%%), %i/%i tablespaces (%-30s)\r",
						totaldone / 1024, totalsize,
						(int) ((totaldone / 1024) * 100 / totalsize),
						rownum, tablespacecount, fn);
			else
				fprintf(stderr, "%llu/%llu kB (%i%%), %i/%i tablespaces\r",
						totaldone / 1024, totalsize,
						(int) ((totaldone / 1024) * 100 / totalsize),
						rownum, tablespacecount);
		}
	}							/* while (1) */

	if (copybuf != NULL)
		PQfreemem(copybuf);
}


static void
ReceiveAndUnpackTarFile(PGconn *conn, PGresult *res, int rownum)
{
	char		current_path[MAXPGPATH];
	char		fn[MAXPGPATH];
	int			current_len_left;
	int			current_padding;
	char	   *copybuf = NULL;
	FILE	   *file = NULL;

	if (PQgetisnull(res, rownum, 0))
		strcpy(current_path, basedir);
	else
		strcpy(current_path, PQgetvalue(res, rownum, 1));

	/*
	 * Make sure we're unpacking into an empty directory
	 */
	verify_dir_is_empty_or_create(current_path);

	if (current_path[0] == '/')
	{
		current_path[0] = '_';
	}

	/*
	 * Get the COPY data
	 */
	res = PQgetResult(conn);
	if (!res || PQresultStatus(res) != PGRES_COPY_OUT)
	{
		fprintf(stderr, _("%s: could not get COPY data stream: %s\n"),
				progname, PQerrorMessage(conn));
		exit(1);
	}

	while (1)
	{
		int			r;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		r = PQgetCopyData(conn, &copybuf, 0);

		if (r == -1)
		{
			/*
			 * End of chunk
			 */
			if (file)
				fclose(file);

			break;
		}
		else if (r == -2)
		{
			fprintf(stderr, _("%s: could not read COPY data: %s\n"),
					progname, PQerrorMessage(conn));
			exit(1);
		}

		if (file == NULL)
		{
			/*
			 * No current file, so this must be the header for a new file
			 */
			if (r != 512)
			{
				fprintf(stderr, _("%s: Invalid tar block header size: %i\n"),
						progname, r);
				exit(1);
			}
			totaldone += 512;

			if (sscanf(copybuf + 124, "%11o", &current_len_left) != 1)
			{
				fprintf(stderr, _("%s: could not parse file size!\n"),
					progname);
				exit(1);
			}

			/*
			 * All files are padded up to 512 bytes
			 */
			current_padding =
				((current_len_left + 511) & ~511) - current_len_left;

			/*
			 * First part of header is zero terminated filename
			 */
			sprintf(fn, "%s/%s", current_path, copybuf);
			if (fn[strlen(fn) - 1] == '/')
			{
				/*
				 * Ends in a slash means directory or symlink to directory
				 */
				if (copybuf[156] == '5')
				{
					/*
					 * Directory
					 */
					fn[strlen(fn) - 1] = '\0';	/* Remove trailing slash */
					if (mkdir(fn, S_IRWXU) != 0)		/* XXX: permissions */
					{
						fprintf(stderr,
								_("%s: could not create directory \"%s\": %m\n"),
								progname, fn);
						exit(1);
					}
				}
				else if (copybuf[156] == '2')
				{
					/*
					 * Symbolic link
					 */
					fn[strlen(fn) - 1] = '\0';	/* Remove trailing slash */
					if (symlink(&copybuf[157], fn) != 0)
					{
						fprintf(stderr,
								_("%s: could not create symbolic link from %s to %s: %m\n"),
								progname, fn, &copybuf[157]);
						exit(1);
					}

					/*
					 * XXX: permissions
					 */
				}
				else
				{
					fprintf(stderr, _("%s: unknown link indicator '%c'\n"),
							progname, copybuf[156]);
					exit(1);
				}
				continue;		/* directory or link handled */
			}

			/*
			 * regular file
			 */
			file = fopen(fn, "wb");		/* XXX: permissions & owner */
			if (!file)
			{
				fprintf(stderr, _("%s: could not create file '%s': %m\n"),
						progname, fn);
				exit(1);
			}

			if (current_len_left == 0)
			{
				/*
				 * Done with this file, next one will be a new tar header
				 */
				fclose(file);
				file = NULL;
				continue;
			}
		}						/* new file */
		else
		{
			/*
			 * Continuing blocks in existing file
			 */
			if (current_len_left == 0 && r == current_padding)
			{
				/*
				 * Received the padding block for this file, ignore it and
				 * close the file, then move on to the next tar header.
				 */
				fclose(file);
				file = NULL;
				totaldone += r;
				continue;
			}

			fwrite(copybuf, r, 1, file);		/* XXX: result code */
			if (showprogress)
			{
				totaldone += r;
				if (verbose)
					fprintf(stderr,
							"%llu/%llu kB (%i%%) %i/%i tablespaces (%-30s)\r",
							totaldone / 1024, totalsize,
							(int) ((totaldone / 1024) * 100 / totalsize),
							rownum, tablespacecount, fn);
				else
					fprintf(stderr, "%llu/%llu kB (%i%%) %i/%i tablespaces\r",
							totaldone / 1024, totalsize,
							(int) ((totaldone / 1024) * 100 / totalsize),
							rownum, tablespacecount);
			}

			current_len_left -= r;
			if (current_len_left == 0 && current_padding == 0)
			{
				/*
				 * Received the last block, and there is no padding to be
				 * expected. Close the file and move on to the next tar
				 * header.
				 */
				fclose(file);
				file = NULL;
				continue;
			}
		}						/* continuing data in existing file */
	}							/* loop over all data blocks */

	if (file != NULL)
	{
		fprintf(stderr, _("%s: last file was never finsihed!\n"), progname);
		exit(1);
	}

	if (copybuf != NULL)
		PQfreemem(copybuf);
}


static PGconn *
GetConnection(void)
{
	char		buf[MAXPGPATH];
	PGconn	   *conn;

	sprintf(buf, "%s dbname=replication replication=true", conninfo);

	if (verbose)
		fprintf(stderr, _("%s: Connecting to \"%s\"\n"), progname, buf);

	conn = PQconnectdb(buf);
	if (!conn || PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, _("%s: could not connect to server: %s\n"),
				progname, PQerrorMessage(conn));
		exit(1);
	}

	return conn;
}

static void
BaseBackup()
{
	PGconn	   *conn;
	PGresult   *res;
	char		current_path[MAXPGPATH];
	int			i;

	/*
	 * Connect in replication mode to the server
	 */
	conn = GetConnection();

	sprintf(current_path, "BASE_BACKUP %s;%s",
			showprogress ? "PROGRESS" : "",
			label);
	if (PQsendQuery(conn, current_path) == 0)
	{
		fprintf(stderr, _("%s: coult not start base backup: %s\n"),
				progname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	/*
	 * Get the header
	 */
	res = PQgetResult(conn);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: could not initiate base backup: %s\n"),
				progname, PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
	if (PQntuples(res) < 1)
	{
		fprintf(stderr, _("%s: no data returned from server.\n"), progname);
		PQfinish(conn);
		exit(1);
	}

	/*
	 * Sum up the total size, for progress reporting
	 */
	totalsize = totaldone = 0;
	tablespacecount = PQntuples(res);
	for (i = 0; i < PQntuples(res); i++)
	{
		if (showprogress)
			totalsize += atol(PQgetvalue(res, i, 2));

		/*
		 * Verify tablespace directories are empty Don't bother with the first
		 * once since it can be relocated, and it will be checked before we do
		 * anything anyway.
		 */
		if (basedir != NULL && i > 0)
			verify_dir_is_empty_or_create(PQgetvalue(res, i, 1));
	}

	/*
	 * When writing to stdout, require a single tablespace
	 */
	if (tardir != NULL && strcmp(tardir, "-") == 0 && PQntuples(res) > 1)
	{
		fprintf(stderr, _("%s: can only write single tablespace to stdout, database has %i.\n"),
				progname, PQntuples(res));
		PQfinish(conn);
		exit(1);
	}

	/*
	 * Start receiving chunks
	 */
	for (i = 0; i < PQntuples(res); i++)
	{
		if (tardir != NULL)
			ReceiveTarFile(conn, res, i);
		else
			ReceiveAndUnpackTarFile(conn, res, i);
	}							/* Loop over all tablespaces */
	PQclear(res);

	if (showprogress)
		fprintf(stderr, "\n");			/* Need to move to next line */

	/*
	 * End of copy data. Final result is already checked inside the loop.
	 */
	PQfinish(conn);

	if (verbose)
		fprintf(stderr, "%s: base backup completed.\n", progname);
}


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"conninfo", required_argument, NULL, 'c'},
		{"basedir", required_argument, NULL, 'd'},
		{"tardir", required_argument, NULL, 't'},
		{"label", required_argument, NULL, 'l'},
		{"verbose", no_argument, NULL, 'v'},
		{"progress", no_argument, NULL, 'p'},
		{NULL, 0, NULL, 0}
	};
	int			c;

	int			option_index;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_basebackup"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
			strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0
				 || strcmp(argv[1], "--version") == 0)
		{
			puts("pg_basebackup (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "c:d:t:l:vp",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'c':
				conninfo = xstrdup(optarg);
				break;
			case 'd':
				basedir = xstrdup(optarg);
				break;
			case 't':
				tardir = xstrdup(optarg);
				break;
			case 'l':
				label = xstrdup(optarg);
				break;
			case 'v':
				verbose++;
				break;
			case 'p':
				showprogress = true;
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
				progname, argv[optind + 1]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Required arguments
	 */
	if (basedir == NULL && tardir == NULL)
	{
		fprintf(stderr, _("%s: no target directory specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (conninfo == NULL)
	{
		fprintf(stderr, _("%s: no conninfo string specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Mutually exclusive arguments
	 */
	if (basedir != NULL && tardir != NULL)
	{
		fprintf(stderr,
				_("%s: both directory mode and tar mode cannot be specified\n"),
				progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Verify directories
	 */
	if (basedir)
		verify_dir_is_empty_or_create(basedir);
	else if (strcmp(tardir, "-") != 0)
		verify_dir_is_empty_or_create(tardir);



	BaseBackup();

	return 0;
}
