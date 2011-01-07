/*-------------------------------------------------------------------------
 *
 * basebackup.c
 *	  code for taking a base backup and streaming it to a standby
 *
 * Portions Copyright (c) 2010-2011, PostgreSQL Global Development Group
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
#include <time.h>

#include "access/xlog_internal.h" /* for pg_start/stop_backup */
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "replication/basebackup.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/elog.h"

static uint64 sendDir(char *path, bool sizeonly);
static void sendFile(char *path, struct stat *statbuf);
static void _tarWriteHeader(char *filename, char *linktarget,
							struct stat *statbuf);
static void SendBackupDirectory(char *location, char *spcoid, bool progress);
static void base_backup_cleanup(int code, Datum arg);

/*
 * Called when ERROR or FATAL happens in SendBaseBackup() after
 * we have started the backup - make sure we end it!
 */
static void
base_backup_cleanup(int code, Datum arg)
{
	do_pg_abort_backup();
}

/*
 * SendBaseBackup() - send a complete base backup.
 *
 * The function will take care of running pg_start_backup() and
 * pg_stop_backup() for the user.
 *
 * It will contain one or more batches. Each batch has a header,
 * in normal result format, followed by a tar format dump in
 * CopyOut format.
 */
void
SendBaseBackup(const char *backup_label, bool progress)
{
	DIR 		   *dir;
	struct dirent  *de;

	/* Make sure we can open the directory with tablespaces in it */
	dir = AllocateDir("pg_tblspc");
	if (!dir)
		ereport(ERROR,
				(errmsg("unable to open directory pg_tblspc: %m")));

	do_pg_start_backup(backup_label, true);

	PG_ENSURE_ERROR_CLEANUP(base_backup_cleanup, (Datum) 0);
	{
		SendBackupDirectory(NULL, NULL, progress);

		/* Check for tablespaces */
		while ((de = ReadDir(dir, "pg_tblspc")) != NULL)
		{
			char fullpath[MAXPGPATH];
			char linkpath[MAXPGPATH];

			if (de->d_name[0] == '.')
				continue;

			snprintf(fullpath, sizeof(fullpath), "pg_tblspc/%s", de->d_name);

			MemSet(linkpath, 0, sizeof(linkpath));
			if (readlink(fullpath, linkpath, sizeof(linkpath) - 1) == -1)
			{
				ereport(WARNING,
						(errmsg("unable to read symbolic link %s", fullpath)));
				continue;
			}

			SendBackupDirectory(linkpath, de->d_name, progress);
		}

		FreeDir(dir);
	}
	PG_END_ENSURE_ERROR_CLEANUP(base_backup_cleanup, (Datum) 0);

	do_pg_stop_backup();
}

static void
send_int8_string(StringInfoData *buf, uint64 intval)
{
	char is[32];
	sprintf(is, INT64_FORMAT, intval);
	pq_sendint(buf, strlen(is), 4);
	pq_sendbytes(buf, is, strlen(is));
}

static void
SendBackupDirectory(char *location, char *spcoid, bool progress)
{
	StringInfoData buf;
	uint64			size = 0;

	if (progress)
	{
		/*
		 * If we're asking for progress, start by counting the size of
		 * the tablespace. If not, we'll send 0.
		 */
		size = sendDir(location == NULL ? "." : location, true);
	}

	/* Construct and send the directory information */
	pq_beginmessage(&buf, 'T'); /* RowDescription */
	pq_sendint(&buf, 3, 2); /* 3 fields */

	/* First field - spcoid */
	pq_sendstring(&buf, "spcoid");
	pq_sendint(&buf, 0, 4); /* table oid */
	pq_sendint(&buf, 0, 2); /* attnum */
	pq_sendint(&buf, OIDOID, 4); /* type oid */
	pq_sendint(&buf, 4, 2); /* typlen */
	pq_sendint(&buf, 0, 4); /* typmod */
	pq_sendint(&buf, 0, 2); /* format code */

	/* Second field - spcpath */
	pq_sendstring(&buf, "spclocation");
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_sendint(&buf, TEXTOID, 4);
	pq_sendint(&buf, -1, 2);
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);

	/* Third field - size */
	pq_sendstring(&buf, "size");
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_sendint(&buf, INT8OID, 4);
	pq_sendint(&buf, 8, 2);
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_endmessage(&buf);

	/* Send one datarow message */
	pq_beginmessage(&buf, 'D');
	pq_sendint(&buf, 3, 2); /* number of columns */
	if (location == NULL)
	{
		pq_sendint(&buf, -1, 4); /* Length = -1 ==> NULL */
		pq_sendint(&buf, -1, 4);
	}
	else
	{
		pq_sendint(&buf, 4, 4); /* Length of oid */
		pq_sendint(&buf, atoi(spcoid), 4);
		pq_sendint(&buf, strlen(location), 4); /* length of text */
		pq_sendbytes(&buf, location, strlen(location));
	}
	send_int8_string(&buf, size/1024);
	pq_endmessage(&buf);

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");

	/* Send CopyOutResponse message */
	pq_beginmessage(&buf, 'H');
	pq_sendbyte(&buf, 0);			/* overall format */
	pq_sendint(&buf, 0, 2);			/* natts */
	pq_endmessage(&buf);

	/* tar up the data directory if NULL, otherwise the tablespace */
	sendDir(location == NULL ? "." : location, false);

	/* Send CopyDone message */
	pq_putemptymessage('c');
}


static uint64
sendDir(char *path, bool sizeonly)
{
	DIR		   *dir;
	struct dirent *de;
	char		pathbuf[MAXPGPATH];
	struct stat statbuf;
	uint64		size = 0;

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
				elog(WARNING, "could not stat file or directory \"%s\": %m",
					 pathbuf);

			/* If the file went away while scanning, it's no error. */
			continue;
		}

		if (S_ISLNK(statbuf.st_mode) && strcmp(path, "./pg_tblspc") == 0)
		{
			/* Allow symbolic links in pg_tblspc */
			char	linkpath[MAXPGPATH];

			MemSet(linkpath, 0, sizeof(linkpath));
			if (readlink(pathbuf, linkpath, sizeof(linkpath)-1) == -1)
			{
				elog(WARNING, "unable to read symbolic link \"%s\": %m",
					 pathbuf);
			}
			if (!sizeonly)
				_tarWriteHeader(pathbuf, linkpath, &statbuf);
		}
		else if (S_ISDIR(statbuf.st_mode))
		{
			/*
			 * Store a directory entry in the tar file so we can get
			 * the permissions right.
			 */
			if (!sizeonly)
				_tarWriteHeader(pathbuf, NULL, &statbuf);

			/* call ourselves recursively for a directory */
			size += sendDir(pathbuf, sizeonly);
		}
		else if (S_ISREG(statbuf.st_mode))
		{
			size += statbuf.st_size;
			if (!sizeonly)
				sendFile(pathbuf, &statbuf);
		}
		else
			elog(WARNING, "skipping special file \"%s\"", pathbuf);
	}
	FreeDir(dir);
	return size;
}

/*****
 * Functions for handling tar file format
 *
 * Copied from pg_dump, but modified to work with libpq for sending
 */


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

/* Given the member, write the TAR header & send the file */
static void
sendFile(char *filename, struct stat *statbuf)
{
	FILE	   *fp;
	char		buf[32768];
	size_t		cnt;
	pgoff_t		len = 0;
	size_t		pad;

	fp = AllocateFile(filename, "rb");
	if (fp == NULL)
		elog(ERROR, "could not open file \"%s\": %m", filename);

	/*
	 * Some compilers will throw a warning knowing this test can never be true
	 * because pgoff_t can't exceed the compared maximum on their platform.
	 */
	if (statbuf->st_size > MAX_TAR_MEMBER_FILELEN)
		elog(ERROR, "archive member too large for tar format");

	_tarWriteHeader(filename, NULL, statbuf);

	while ((cnt = fread(buf, 1, Min(sizeof(buf), statbuf->st_size - len), fp)) > 0)
	{
		/* Send the chunk as a CopyData message */
		pq_putmessage('d', buf, cnt);
		len += cnt;

		if (len >= statbuf->st_size)
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
	if (len < statbuf->st_size)
	{
		MemSet(buf, 0, sizeof(buf));
		while(len < statbuf->st_size)
		{
			cnt = Min(sizeof(buf), statbuf->st_size - len);
			pq_putmessage('d', buf, cnt);
			len += cnt;
		}
	}

	/* Pad to 512 byte boundary, per tar format requirements */
	pad = ((len + 511) & ~511) - len;
	if (pad > 0)
	{
		MemSet(buf, 0, pad);
		pq_putmessage('d', buf, pad);
	}

	FreeFile(fp);
}


static void
_tarWriteHeader(char *filename, char *linktarget, struct stat *statbuf)
{
	char		h[512];
	int			lastSum = 0;
	int			sum;

	memset(h, 0, sizeof(h));

	/* Name 100 */
	sprintf(&h[0], "%.99s", filename);
	if (linktarget != NULL || S_ISDIR(statbuf->st_mode))
	{
		/*
		 * We only support symbolic links to directories, and this is indicated
		 * in the tar format by adding a slash at the end of the name, the same
		 * as for regular directories.
		 */
		h[strlen(filename)] = '/';
		h[strlen(filename)+1] = '\0';
	}

	/* Mode 8 */
	sprintf(&h[100], "%07o ", statbuf->st_mode);

	/* User ID 8 */
	sprintf(&h[108], "%07o ", statbuf->st_uid);

	/* Group 8 */
	sprintf(&h[117], "%07o ", statbuf->st_gid);

	/* File size 12 - 11 digits, 1 space, no NUL */
	if (linktarget != NULL || S_ISDIR(statbuf->st_mode))
		/* Symbolic link or directory has size zero */
		print_val(&h[124], 0, 8, 11);
	else
		print_val(&h[124], statbuf->st_size, 8, 11);
	sprintf(&h[135], " ");

	/* Mod Time 12 */
	sprintf(&h[136], "%011o ", (int) statbuf->st_mtime);

	/* Checksum 8 */
	sprintf(&h[148], "%06o ", lastSum);

	if (linktarget != NULL)
	{
		/* Type - Symbolic link */
		sprintf(&h[156], "2");
		strcpy(&h[157], linktarget);
	}
	else if (S_ISDIR(statbuf->st_mode))
		/* Type - directory */
		sprintf(&h[156], "5");
	else
		/* Type - regular file */
		sprintf(&h[156], "0");

	/* Link tag 100 (NULL) */

	/* Magic 6 + Version 2 */
	sprintf(&h[257], "ustar00");

	/* User 32 */
	/* XXX: Do we need to care about setting correct username? */
	sprintf(&h[265], "%.31s", "postgres");

	/* Group 32 */
	/* XXX: Do we need to care about setting correct group name? */
	sprintf(&h[297], "%.31s", "postgres");

	/* Maj Dev 8 */
	sprintf(&h[329], "%6o ", 0);

	/* Min Dev 8 */
	sprintf(&h[337], "%6o ", 0);

	while ((sum = _tarChecksum(h)) != lastSum)
	{
		sprintf(&h[148], "%06o ", sum);
		lastSum = sum;
	}

	pq_putmessage('d', h, 512);
}
