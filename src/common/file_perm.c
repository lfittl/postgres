/*-------------------------------------------------------------------------
 *
 * File and directory permission routines
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/file_perm.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include "common/file_perm.h"

/* Modes for creating directories and files in the data directory */
int			pg_dir_create_mode = PG_DIR_MODE_OWNER;
int			pg_file_create_mode = PG_FILE_MODE_OWNER;

/*
 * Mode mask to pass to umask().  This is more of a preventative measure since
 * all file/directory creates should be performed using the create modes above.
 */
int			pg_mode_mask = PG_MODE_MASK_OWNER;

/*
 * Set create modes and mask to use when writing to PGDATA based on the data
 * directory mode passed.  If group read/execute are present in the mode, then
 * create modes and mask will be relaxed to allow group read/execute on all
 * newly created files and directories.
 */
void
SetDataDirectoryCreatePerm(int dataDirMode)
{
	/* If the data directory mode has group access */
	if ((PG_DIR_MODE_GROUP & dataDirMode) == PG_DIR_MODE_GROUP)
	{
		pg_dir_create_mode = PG_DIR_MODE_GROUP;
		pg_file_create_mode = PG_FILE_MODE_GROUP;
		pg_mode_mask = PG_MODE_MASK_GROUP;
	}
	/* Else use default permissions */
	else
	{
		pg_dir_create_mode = PG_DIR_MODE_OWNER;
		pg_file_create_mode = PG_FILE_MODE_OWNER;
		pg_mode_mask = PG_MODE_MASK_OWNER;
	}
}

#ifdef FRONTEND

/*
 * Get the create modes and mask to use when writing to PGDATA by examining the
 * mode of the PGDATA directory and calling SetDataDirectoryCreatePerm().
 *
 * Errors are not handled here and should be reported by the application when
 * false is returned.
 *
 * Suppress when on Windows, because there may not be proper support for Unix-y
 * file permissions.  But we still run stat() on the directory so that callers
 * get consistent behavior for example if the directory does not exist.
 */
bool
GetDataDirectoryCreatePerm(const char *dataDir)
{
	struct stat statBuf;

	/*
	 * If an error occurs getting the mode then return false.  The caller is
	 * responsible for generating an error, if appropriate, indicating that we
	 * were unable to access the data directory.
	 */
	if (stat(dataDir, &statBuf) == -1)
		return false;

#if !defined(WIN32) && !defined(__CYGWIN__)
	/* Set permissions */
	SetDataDirectoryCreatePerm(statBuf.st_mode);
#endif

	return true;
}


#endif							/* FRONTEND */
