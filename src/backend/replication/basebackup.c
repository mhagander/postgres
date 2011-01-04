/*-------------------------------------------------------------------------
 *
 * basebackup.c
 *	  code for taking a base backup and streaming it a standby
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
#include "utils/builtins.h"
#include "utils/elog.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "replication/basebackup.h"
#include "storage/fd.h"

static void sendDir(char *path);
static void sendFile(char *path);
static void _tarWriteHeader(char *filename, char *linktarget,
							struct stat *statbuf);
static void SendBackupDirectory(char *location, char *spcoid);

/*
 * SendBaseBackup() - send a complete base backup.
 *
 * The function will take care of running pg_start_backup() and
 * pg_stop_backup() for the user.
 *
 * It will contain one or more batches. Each batch has a header,
 * followed by a tar format dump.
 *
 * Batch header is simple:
 * <tablespaceoid>;<tablespacepath>\0
 *
 * The ... is left for future additions - it should *not* be assumed
 * to be empty.
 *
 * Both name and path are left empty for the PGDATA batch.
 */
void
SendBaseBackup(const char *backup_label)
{
	DIR *dir;
	struct dirent *de;

	/* Make sure we can open the directory with tablespaces in it */
	dir = AllocateDir("pg_tblspc");
	if (!dir)
		ereport(ERROR,
				(errmsg("unable to open directory pg_tblspc: %m")));

	do_pg_start_backup(backup_label, true);

	SendBackupDirectory(NULL, NULL);

	/* Check for tablespaces */
	while ((de = ReadDir(dir, "pg_tblspc")) != NULL)
	{
		char fullpath[MAXPGPATH];
		char linkpath[MAXPGPATH];

		if (de->d_name[0] == '.')
			continue;

		sprintf(fullpath, "pg_tblspc/%s", de->d_name);

		MemSet(linkpath, 0, sizeof(linkpath));
		if (readlink(fullpath, linkpath, sizeof(linkpath) - 1) == -1)
		{
			ereport(WARNING, (errmsg("unable to read symbolic link %s", fullpath)));
			continue;
		}

		SendBackupDirectory(linkpath, de->d_name);
	}

	FreeDir(dir);

	do_pg_stop_backup();
}

static
void SendBackupDirectory(char *location, char *spcoid)
{
	StringInfoData buf;

	/* Send CopyOutResponse message */
	pq_beginmessage(&buf, 'H');
	pq_sendbyte(&buf, 0);		/* overall format */
	pq_sendint(&buf, 0, 2);			/* natts */
	pq_endmessage(&buf);

	/* Construct and send the directory information */
	initStringInfo(&buf);
	if (location == NULL)
		appendStringInfoString(&buf, ";");
	else
		appendStringInfo(&buf, "%s;%s", spcoid, location);
	appendStringInfoChar(&buf, '\0');
	pq_putmessage('d', buf.data, buf.len);

	if (location == NULL)
		/* tar up the data directory */
		sendDir(".");
	else
		/* tar up the tablespace */
		sendDir(location);

	/* Send CopyDone message */
	pq_putemptymessage('c');
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

		if (S_ISLNK(statbuf.st_mode) && strcmp(path, "./pg_tblspc") == 0)
		{
			/* Allow symbolic links in pg_tblspc */
			char	linkpath[MAXPGPATH];

			MemSet(linkpath, 0, sizeof(linkpath));
			if (readlink(pathbuf, linkpath, sizeof(linkpath)-1) == -1)
			{
				elog(WARNING, "Unable to read symbolic link \"%s\": %m",
					 pathbuf);
			}
			_tarWriteHeader(pathbuf, linkpath, &statbuf);
		}
		else if (S_ISDIR(statbuf.st_mode))
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
sendFile(char *filename)
{
	FILE	   *fp;
	char		buf[32768];
	size_t		cnt;
	pgoff_t		len = 0;
	size_t		pad;
	struct stat statbuf;

	if (lstat(filename, &statbuf) != 0)
		elog(ERROR, "could not stat file \"%s\": %m", filename);

	fp = AllocateFile(filename, "rb");
	if (fp == NULL)
		elog(ERROR, "could not open file \"%s\": %m", filename);

	/*
	 * Some compilers will throw a warning knowing this test can never be true
	 * because pgoff_t can't exceed the compared maximum on their platform.
	 */
	if (statbuf.st_size > MAX_TAR_MEMBER_FILELEN)
		elog(ERROR, "archive member too large for tar format");

	_tarWriteHeader(filename, NULL, &statbuf);

	while ((cnt = fread(buf, 1, Min(sizeof(buf), statbuf.st_size - len), fp)) > 0)
	{
		/* Send the chunk as a CopyData message */
		pq_putmessage('d', buf, cnt);
		len += cnt;

		if (len >= statbuf.st_size)
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
	if (len < statbuf.st_size)
	{
		MemSet(buf, 0, sizeof(buf));
		while(len < statbuf.st_size)
		{
			cnt = Min(sizeof(buf), statbuf.st_size - len);
			pq_putmessage('d', buf, cnt);
			len += cnt;
		}
	}

	/* Pad to 512 byte boundary */
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
	if (linktarget != NULL)
	{
		/*
		 * We only support symbolic links to directories, and this is indicated
		 * in the tar format by adding a slash at the end of the name.
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
	print_val(&h[124], (linktarget==NULL)?statbuf->st_size:0, 8, 11);
	sprintf(&h[135], " ");

	/* Mod Time 12 */
	sprintf(&h[136], "%011o ", (int) statbuf->st_mtime);

	/* Checksum 8 */
	sprintf(&h[148], "%06o ", lastSum);

	if (linktarget != NULL)
	{
		/* Symbolic link */
		sprintf(&h[156], "2");
		strcpy(&h[157], linktarget);
	}
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
