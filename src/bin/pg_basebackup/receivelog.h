#include "access/xlogdefs.h"

/*
 * Called before trying to read more data or when a segment is
 * finished. Also called when the streaming stops to check
 * whether the current log segment can be treated as a complete one.
 */
typedef bool (*stream_continue_callback)(XLogRecPtr segendpos, uint32 timeline);

extern bool ReceiveXlogStream(PGconn *conn,
							  XLogRecPtr startpos,
							  uint32 timeline,
							  char *sysidentifier,
							  char *basedir,
							  stream_continue_callback stream_continue,
							  int standby_message_timeout);
