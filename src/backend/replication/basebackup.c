/*-------------------------------------------------------------------------
 *
 * basebackup.c
 *	  code for taking a base backup and streaming it a standby
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

#include "access/xlog_internal.h" /* for pg_start/stop_backup */
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "replication/basebackup.h"
#include "storage/fd.h"

static void sendDir(char *path);
static void sendFile(char *path);
static void _tarWriteHeader(char *filename, uint64 fileLen);

void
SendBaseBackup(void)
{
	StringInfoData buf;

	DirectFunctionCall2(&pg_start_backup, CStringGetTextDatum("basebackup"),
						BoolGetDatum(true));

	/* Send CopyOutResponse message */
	pq_beginmessage(&buf, 'H');
	pq_sendbyte(&buf, 0);		/* overall format */
	pq_sendint(&buf, 0, 2);			/* natts */
	pq_endmessage(&buf);

	/* tar up the data directory */
	sendDir(".");

	/* Send CopyDone message */
	pq_putemptymessage('c');

	/* XXX: Is there no DirectFunctionCall0? */
	DirectFunctionCall1(&pg_stop_backup, (Datum) 0);
}

static void
sendDir(char *path)
{
	DIR		   *dir;
	struct dirent *de;
	char		pathbuf[MAXPGPATH];
	struct stat statbuf;

	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		/* Skip special stuff */
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(pathbuf, MAXPGPATH, "%s/%s", path, de->d_name);

		/* Skip pg_xlog and postmaster.pid */
		if (strcmp(pathbuf, "./pg_xlog") == 0)
			continue;
		if (strcmp(pathbuf, "./postmaster.pid") == 0)
			continue;

		if (lstat(pathbuf, &statbuf) != 0)
		{
			if (errno != ENOENT)
			{
				elog(WARNING, "could not stat file or directory \"%s\": %m",
					 pathbuf);
			}
			continue;
		}

		if (S_ISDIR(statbuf.st_mode))
		{
			/* call ourselves recursively for a directory */
			sendDir(pathbuf);
		}
		else if (S_ISREG(statbuf.st_mode))
		{
			sendFile(pathbuf);
		}
		else
			elog(WARNING, "skipping special file \"%s\"", pathbuf);
	}
	FreeDir(dir);
}


/***** Functions for handling tar file format, copied from pg_dump ******/

/*
 * Utility routine to print possibly larger than 32 bit integers in a
 * portable fashion.  Filled with zeros.
 */
static void
print_val(char *s, uint64 val, unsigned int base, size_t len)
{
	int			i;

	for (i = len; i > 0; i--)
	{
		int			digit = val % base;

		s[i - 1] = '0' + digit;
		val = val / base;
	}
}

/*
 * Maximum file size for a tar member: The limit inherent in the
 * format is 2^33-1 bytes (nearly 8 GB).  But we don't want to exceed
 * what we can represent in pgoff_t.
 */
#define MAX_TAR_MEMBER_FILELEN (((int64) 1 << Min(33, sizeof(pgoff_t)*8 - 1)) - 1)

static int
_tarChecksum(char *header)
{
	int			i,
				sum;

	sum = 0;
	for (i = 0; i < 512; i++)
		if (i < 148 || i >= 156)
			sum += 0xFF & header[i];
	return sum + 256;			/* Assume 8 blanks in checksum field */
}

/* Given the member, write the TAR header & copy the file */
static void
sendFile(char *filename)
{
	FILE	   *fp;
	char		buf[32768];
	size_t		cnt;
	pgoff_t		len = 0;
	size_t		pad;
	pgoff_t		fileLen;

	fp = AllocateFile(filename, "rb");
	if (fp == NULL)
		elog(ERROR, "could not open file \"%s\": %m", filename);

	/*
	 * Find file len & go back to start.
	 */
	fseeko(fp, 0, SEEK_END);
	fileLen = ftello(fp);
	fseeko(fp, 0, SEEK_SET);

	/*
	 * Some compilers will throw a warning knowing this test can never be true
	 * because pgoff_t can't exceed the compared maximum on their platform.
	 */
	if (fileLen > MAX_TAR_MEMBER_FILELEN)
		elog(ERROR, "archive member too large for tar format");

	_tarWriteHeader(filename, fileLen);

	while ((cnt = fread(buf, 1, Min(sizeof(buf), fileLen - len), fp)) > 0)
	{
		/* Send the chunk as a CopyData message */
		pq_putmessage('d', buf, cnt);
		len += cnt;

		if (len >= fileLen)
		{
			/*
			 * Reached end of file. The file could be longer, if it was
			 * extended while we were sending it, but for a base backup we
			 * can ignore such extended data. It will be restored from WAL.
			 */
			break;
		}
	}
	/* If the file was truncated while we were sending it, pad it with zeros */
	if (len < fileLen)
	{
		MemSet(buf, 0, sizeof(buf));
		while(len < fileLen)
		{
			cnt = Min(sizeof(buf), fileLen - len);
			pq_putmessage('d', buf, cnt);
			len += cnt;
		}
	}

	/* Pad to 512 byte boundary */
	pad = ((len + 511) & ~511) - len;
	MemSet(buf, 0, pad);
	pq_putmessage('d', buf, pad);

	FreeFile(fp);
}


static void
_tarWriteHeader(char *filename, uint64 fileLen)
{
	char		h[512];
	int			lastSum = 0;
	int			sum;

	memset(h, 0, sizeof(h));

	/* Name 100 */
	sprintf(&h[0], "%.99s", filename);

	/* Mode 8 */
	sprintf(&h[100], "100600 ");

	/* User ID 8 */
	sprintf(&h[108], "004000 ");

	/* Group 8 */
	sprintf(&h[116], "002000 ");

	/* File size 12 - 11 digits, 1 space, no NUL */
	print_val(&h[124], fileLen, 8, 11);
	sprintf(&h[135], " ");

	/* Mod Time 12 */
	sprintf(&h[136], "%011o ", (int) time(NULL));

	/* Checksum 8 */
	sprintf(&h[148], "%06o ", lastSum);

	/* Type - regular file */
	sprintf(&h[156], "0");

	/* Link tag 100 (NULL) */

	/* Magic 6 + Version 2 */
	sprintf(&h[257], "ustar00");

#if 0
	/* User 32 */
	sprintf(&h[265], "%.31s", "");		/* How do I get username reliably? Do
										 * I need to? */

	/* Group 32 */
	sprintf(&h[297], "%.31s", "");		/* How do I get group reliably? Do I
										 * need to? */

	/* Maj Dev 8 */
	sprintf(&h[329], "%6o ", 0);

	/* Min Dev 8 */
	sprintf(&h[337], "%6o ", 0);
#endif

	while ((sum = _tarChecksum(h)) != lastSum)
	{
		sprintf(&h[148], "%06o ", sum);
		lastSum = sum;
	}

	pq_putmessage('d', h, 512);
}
