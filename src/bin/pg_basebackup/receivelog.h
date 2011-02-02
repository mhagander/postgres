/*
 * Called whenever a segment is finished, return true to stop
 * the streaming at this point.
 */
typedef bool (*segment_finish_callback)(XLogRecPtr segendpos);

bool ReceiveXlogStream(PGconn *conn,
					   XLogRecPtr startpos,
					   int timeline,
					   char *basedir,
					   segment_finish_callback segment_finish);
