#include "access/xlogdefs.h"

/*
 * Called whenever a segment is finished, return true to stop
 * the streaming at this point.
 */
typedef bool (*segment_finish_callback)(XLogRecPtr segendpos, uint32 timeline);

bool ReceiveXlogStream(PGconn *conn,
					   XLogRecPtr startpos,
					   uint32 timeline,
					   char *basedir,
					   segment_finish_callback segment_finish);
