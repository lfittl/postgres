/* ----------
 * pgstat.c
 *
 *	Statistics collector facility.
 *
 *  Collects per-table and per-function usage statistics of backends and shares
 *  them among all backends via shared memory. Every backend records
 *  individual activity in local memory using pg_count_*() and friends
 *  interfaces during a transaction. Then pgstat_report_stat() is called at
 *  the end of a transaction to flush out the local numbers to shared
 *  memory. To avoid congestion on the shared memory, we do that not often
 *  than PGSTAT_STAT_MIN_INTERVAL(500ms). Still it is possible that a backend
 *  cannot flush all or a part of local numbers immediately, such numbers are
 *  postponed to the next chances with the interval of
 *  PGSTAT_STAT_RETRY_INTERVAL(100ms), but they are not kept longer than
 *  PGSTAT_STAT_MAX_INTERVAL(1000ms).
 *
 *	Copyright (c) 2001-2019, PostgreSQL Global Development Group
 *
 *	src/backend/postmaster/pgstat.c
 * ----------
 */
#include "postgres.h"

#include <unistd.h>

#include "pgstat.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "catalog/pg_proc.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "utils/ascii.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/probes.h"
#include "utils/snapmgr.h"

/* ----------
 * Timer definitions.
 * ----------
 */

#define PGSTAT_STAT_MIN_INTERVAL	500 /* Minimum time between stats data
  										 * updates; in milliseconds. */

#define PGSTAT_STAT_RETRY_INTERVAL	100 /* Retry interval between after
 										 * elapsed PGSTAT_MIN_INTERVAL */

#define PGSTAT_STAT_MAX_INTERVAL   1000 /* Maximum time between stats data
 										 * updates; in milliseconds. */

/* ----------
 * The initial size hints for the hash tables used in the collector.
 * ----------
 */
#define PGSTAT_TAB_HASH_SIZE	512
#define PGSTAT_FUNCTION_HASH_SIZE	512


/* ----------
 * Total number of backends including auxiliary
 *
 * We reserve a slot for each possible BackendId, plus one for each
 * possible auxiliary process type.  (This scheme assumes there is not
 * more than one of any auxiliary process type at a time.) MaxBackends
 * includes autovacuum workers and background workers as well.
 * ----------
 */
#define NumBackendStatSlots (MaxBackends + NUM_AUXPROCTYPES)

/*
 * Operation mode of pgstat_get_db_entry.
 */
#define	PGSTAT_FETCH_SHARED		0
#define	PGSTAT_FETCH_EXCLUSIVE	1
#define	PGSTAT_FETCH_NOWAIT		2

typedef enum PgStat_TableLookupState
{
	PGSTAT_ENTRY_NOT_FOUND,
	PGSTAT_ENTRY_FOUND,
	PGSTAT_ENTRY_LOCK_FAILED
} PgStat_TableLookupState;

/* ----------
 * GUC parameters
 * ----------
 */
bool		pgstat_track_activities = false;
bool		pgstat_track_counts = false;
int			pgstat_track_functions = TRACK_FUNC_OFF;
int			pgstat_track_activity_query_size = 1024;

/*
 * This used to be a GUC variable and is no longer used in this file, but left
 * alone just for backward comptibility for extensions, having the default
 * value.
 */
char	   *pgstat_stat_directory = PG_STAT_TMP_DIR;

LWLock		StatsMainLock;
#define		StatsLock (&StatsMainLock)

/* Shared stats bootstrap information */
typedef struct StatsShmemStruct {
	dsa_handle stats_dsa_handle;
	dshash_table_handle db_hash_handle;
	dsa_pointer	global_stats;
	dsa_pointer	archiver_stats;
	TimestampTz last_update;
} StatsShmemStruct;

/*
 * BgWriter global statistics counters (unused in other processes).
 * Stored directly in a stats message structure so it can be sent
 * without needing to copy things around.  We assume this inits to zeroes.
 */
PgStat_MsgBgWriter BgWriterStats;

/* ----------
 * Local data
 * ----------
 */
/* Variables lives for the backend lifetime */
static StatsShmemStruct * StatsShmem = NULL;
static dsa_area *area = NULL;
static dshash_table *pgStatDBHash;
static MemoryContext pgSharedStatsContext = NULL;

/* dshash parameter for each type of table */
static const dshash_parameters dsh_dbparams = {
	sizeof(Oid),
	sizeof(PgStat_StatDBEntry),
	dshash_memcmp,
	dshash_memhash,
	LWTRANCHE_STATS
};
static const dshash_parameters dsh_tblparams = {
	sizeof(Oid),
	sizeof(PgStat_StatTabEntry),
	dshash_memcmp,
	dshash_memhash,
	LWTRANCHE_STATS
};
static const dshash_parameters dsh_funcparams = {
	sizeof(Oid),
	sizeof(PgStat_StatFuncEntry),
	dshash_memcmp,
	dshash_memhash,
	LWTRANCHE_STATS
};

/*
 * Structures in which backends store per-table info that's waiting to be
 * written to shared memory.
 *
 * NOTE: once allocated, TabStatusArray structures are never moved or deleted
 * for the life of the backend.  Also, we zero out the t_id fields of the
 * contained PgStat_TableStatus structs whenever they are not actively in use.
 * This allows relcache pgstat_info pointers to be treated as long-lived data,
 * avoiding repeated searches in pgstat_initstats() when a relation is
 * repeatedly opened during a transaction.
 */
#define TABSTAT_QUANTUM		100 /* we alloc this many at a time */

typedef struct TabStatusArray
{
	struct TabStatusArray *tsa_next;	/* link to next array, if any */
	int			tsa_used;		/* # entries currently used */
	PgStat_TableStatus tsa_entries[TABSTAT_QUANTUM];	/* per-table data */
} TabStatusArray;

static TabStatusArray *pgStatTabList = NULL;

/*
 * pgStatTabHash entry: map from relation OID to PgStat_TableStatus pointer
 */
typedef struct TabStatHashEntry
{
	Oid			t_id;
	PgStat_TableStatus *tsa_entry;
} TabStatHashEntry;

/*
 * Hash table for O(1) t_id -> tsa_entry lookup
 */
static HTAB *pgStatTabHash = NULL;

/*
 * Backends store per-function info that's waiting to be flushed out to shared
 * memory in this hash table (indexed by function OID).
 */
static HTAB *pgStatFunctions = NULL;

/*
 * Indicates if backend has some function stats that it hasn't yet
 * sent to the collector.
 */
static bool have_function_stats = false;

/* dbentry has some additional data in snapshot */
typedef struct PgStat_StatDBEntry_snapshot
{
	PgStat_StatDBEntry shared_part;

	HTAB *snapshot_tables;				/* table entry snapshot */
	HTAB *snapshot_functions;			/* function entry snapshot */
	dshash_table	*dshash_tables;		/* attached tables dshash */
	dshash_table	*dshash_functions;	/* attached functions dshash */
} PgStat_StatDBEntry_snapshot;

/* context struct for snapshot_statentry */
typedef struct pgstat_snapshot_cxt
{
	char		   *hash_name;			/* name of the snapshot hash */
	HTAB		  **hash;				/* placeholder for the hash */
	int				hash_entsize;		/* element size of hash entry */
	dshash_table  **dshash;				/* placeholder for attached dshash */
	dshash_table_handle	dsh_handle;		/* dsh handle to attach */
	const dshash_parameters *dsh_params;/* dshash params */
} pgstat_snapshot_cxt;

/*
 *  Backends store various database-wide info that's waiting to be flushed out
 *  to shared memory in these variables.
 */
static int		n_deadlocks = 0;
static int		n_checksum_failures = 0;
static size_t	n_tmpfiles = 0;
static size_t	n_tmpfilesize = 0;

/*
 * have_recovery_conflicts represents the existence of any kind if conflict
 */
static bool		have_recovery_conflicts = false;
static int		n_conflict_tablespace = 0;
static int		n_conflict_lock = 0;
static int		n_conflict_snapshot = 0;
static int		n_conflict_bufferpin = 0;
static int		n_conflict_startup_deadlock = 0;

/*
 * Tuple insertion/deletion counts for an open transaction can't be propagated
 * into PgStat_TableStatus counters until we know if it is going to commit
 * or abort.  Hence, we keep these counts in per-subxact structs that live
 * in TopTransactionContext.  This data structure is designed on the assumption
 * that subxacts won't usually modify very many tables.
 */
typedef struct PgStat_SubXactStatus
{
	int			nest_level;		/* subtransaction nest level */
	struct PgStat_SubXactStatus *prev;	/* higher-level subxact if any */
	PgStat_TableXactStatus *first;	/* head of list for this subxact */
} PgStat_SubXactStatus;

static PgStat_SubXactStatus *pgStatXactStack = NULL;

static int	pgStatXactCommit = 0;
static int	pgStatXactRollback = 0;
PgStat_Counter pgStatBlockReadTime = 0;
PgStat_Counter pgStatBlockWriteTime = 0;

/* Record that's written to 2PC state file when pgstat state is persisted */
typedef struct TwoPhasePgStatRecord
{
	PgStat_Counter tuples_inserted; /* tuples inserted in xact */
	PgStat_Counter tuples_updated;	/* tuples updated in xact */
	PgStat_Counter tuples_deleted;	/* tuples deleted in xact */
	PgStat_Counter inserted_pre_trunc;	/* tuples inserted prior to truncate */
	PgStat_Counter updated_pre_trunc;	/* tuples updated prior to truncate */
	PgStat_Counter deleted_pre_trunc;	/* tuples deleted prior to truncate */
	Oid			t_id;			/* table's OID */
	bool		t_shared;		/* is it a shared catalog? */
	bool		t_truncated;	/* was the relation truncated? */
} TwoPhasePgStatRecord;

/* Variables for backend status snapshot. The snapshot includes auxiliary. */
static MemoryContext pgStatLocalContext = NULL;
static LocalPgBackendStatus *localBackendStatusTable = NULL;
static int	localNumBackends = 0;

/* Variables for activity statistics snapshot. */
static MemoryContext pgStatSnapshotContext = NULL;
static HTAB *pgStatDBEntrySnapshot;
static TimestampTz snapshot_expires_at = 0; /* local cache expiration time */
static bool		first_in_xact = true;	  /* is the first time in this xact? */


/* Context struct for flushing to shared memory */
typedef struct pgstat_flush_stat_context
{
	int	shgeneration;
	PgStat_StatDBEntry *shdbentry;
	dshash_table *shdb_tabhash;

	int	mygeneration;
	PgStat_StatDBEntry *mydbentry;
	dshash_table *mydb_tabhash;
} pgstat_flush_stat_context;

/*
 * Cluster wide statistics.
 *
 * Contains statistics that are not collected per database or per table.
 * shared_* are the statistics maintained by shared statistics code and
 * snapshot_* are backend snapshots.
 */
static PgStat_ArchiverStats *shared_archiverStats;
static PgStat_ArchiverStats *snapshot_archiverStats;
static PgStat_GlobalStats *shared_globalStats;
static PgStat_GlobalStats *snapshot_globalStats;

/*
 * Total time charged to functions so far in the current backend.
 * We use this to help separate "self" and "other" time charges.
 * (We assume this initializes to zero.)
 */
static instr_time total_func_time;


/* ----------
 * Local function forward declarations
 * ----------
 */

static void pgstat_beshutdown_hook(int code, Datum arg);
static PgStat_StatDBEntry *pgstat_get_db_entry(Oid databaseid, int op,
									PgStat_TableLookupState *status);
static PgStat_StatTabEntry *pgstat_get_tab_entry(dshash_table *table, Oid tableoid, bool create);
static void pgstat_write_pgStatDBHashfile(PgStat_StatDBEntry *dbentry);
static void pgstat_read_pgStatDBHashfile(PgStat_StatDBEntry *dbentry);
static void pgstat_read_current_status(void);
static bool pgstat_flush_stat(pgstat_flush_stat_context *cxt, bool nowait);
static bool pgstat_flush_tabstat(pgstat_flush_stat_context *cxt, bool nowait,
								 PgStat_TableStatus *entry);
static bool pgstat_flush_funcstats(pgstat_flush_stat_context *cxt, bool nowait);
static bool pgstat_flush_miscstats(pgstat_flush_stat_context *cxt, bool nowait);
static bool pgstat_update_tabentry(dshash_table *tabhash,
								   PgStat_TableStatus *stat, bool nowait);
static void pgstat_update_dbentry(PgStat_StatDBEntry *dbentry,
								  PgStat_TableStatus *stat);
static HTAB *pgstat_collect_oids(Oid catalogid, AttrNumber anum_oid);

static PgStat_TableStatus *get_tabstat_entry(Oid rel_id, bool isshared);

static void pgstat_setup_memcxt(void);
static void pgstat_flush_recovery_conflict(PgStat_StatDBEntry *dbentry);
static void pgstat_flush_deadlock(PgStat_StatDBEntry *dbentry);
static void pgstat_flush_checksum_failures(PgStat_StatDBEntry *dbentry);
static void pgstat_flush_tempfile(PgStat_StatDBEntry *dbentry);
static HTAB *create_tabstat_hash(void);
static PgStat_SubXactStatus *get_tabstat_stack_level(int nest_level);
static void add_tabstat_xact_level(PgStat_TableStatus *pgstat_info, int nest_level);
static PgStat_StatFuncEntry *pgstat_fetch_stat_funcentry_extended(PgStat_StatDBEntry *dbent, Oid funcid);
static void pgstat_snapshot_global_stats(void);

static const char *pgstat_get_wait_activity(WaitEventActivity w);
static const char *pgstat_get_wait_client(WaitEventClient w);
static const char *pgstat_get_wait_ipc(WaitEventIPC w);
static const char *pgstat_get_wait_timeout(WaitEventTimeout w);
static const char *pgstat_get_wait_io(WaitEventIO w);

/* ------------------------------------------------------------
 * Local support functions follow
 * ------------------------------------------------------------
 */
static int pin_hashes(PgStat_StatDBEntry *dbentry);
static void unpin_hashes(PgStat_StatDBEntry *dbentry, int generation);
static dshash_table *attach_table_hash(PgStat_StatDBEntry *dbent, int gen);
static dshash_table *attach_function_hash(PgStat_StatDBEntry *dbent, int gen);
static void reset_dbentry_counters(PgStat_StatDBEntry *dbentry);

/* ------------------------------------------------------------
 * Public functions called from postmaster follow
 * ------------------------------------------------------------
 */

static void
pgstat_postmaster_shutdown(int code, Datum arg)
{
	/* trash the stats on crash */
	if (code == 0)
		pgstat_write_statsfiles();
}

Size
StatsShmemSize(void)
{
	return sizeof(StatsShmemStruct);
}

void
StatsShmemInit(void)
{
	bool	found;

	StatsShmem = (StatsShmemStruct *)
		ShmemInitStruct("Stats area", StatsShmemSize(),
 						&found);
	if (!IsUnderPostmaster)
	{
 		Assert(!found);

		StatsShmem->stats_dsa_handle = DSM_HANDLE_INVALID;

		/* Load saved data if any */
		pgstat_read_statsfiles();

		/* need to be called before dsm shutodwn */
		before_shmem_exit(pgstat_postmaster_shutdown, (Datum) 0);
	}

	LWLockInitialize(StatsLock, LWTRANCHE_STATS);
}

/* ----------
 * pgstat_create_shared_stats() -
 *
 *	create shared stats memory
 * ----------
 */
static void
pgstat_create_shared_stats(void)
{
 	MemoryContext oldcontext;

 	Assert(StatsShmem->stats_dsa_handle == DSM_HANDLE_INVALID);

 	/* lives for the lifetime of the process */
 	oldcontext = MemoryContextSwitchTo(pgSharedStatsContext);

 	area = dsa_create(LWTRANCHE_STATS);
 	dsa_pin_mapping(area);

 	/* create the database hash */
	pgStatDBHash = dshash_create(area, &dsh_dbparams, 0);

	/* create shared area and write bootstrap information */
	StatsShmem->stats_dsa_handle = dsa_get_handle(area);
	StatsShmem->global_stats =
		dsa_allocate0(area, sizeof(PgStat_GlobalStats));
	StatsShmem->archiver_stats =
		dsa_allocate0(area, sizeof(PgStat_ArchiverStats));
	StatsShmem->db_hash_handle =
		dshash_get_hash_table_handle(pgStatDBHash);
	StatsShmem->last_update = 0;

	/* initial connect to the memory */
	pgStatDBEntrySnapshot = NULL;
	shared_globalStats = (PgStat_GlobalStats *)
		dsa_get_address(area, StatsShmem->global_stats);
	shared_archiverStats = (PgStat_ArchiverStats *)
		dsa_get_address(area, StatsShmem->archiver_stats);
	MemoryContextSwitchTo(oldcontext);
}

/*
 * pgstat_reset_all() -
 *
 * Clear on-memory counters.  This is currently used only if WAL recovery is
 * needed after a crash.
 */
void
pgstat_reset_all(void)
{
	dshash_seq_status dshstat;
	PgStat_StatDBEntry		   *dbentry;

	Assert (pgStatDBHash);

	dshash_seq_init(&dshstat, pgStatDBHash, false, true);
	while ((dbentry = (PgStat_StatDBEntry *) dshash_seq_next(&dshstat)) != NULL)
	{
		/*
		 * Reset database-level stats, too.  This creates empty hash tables
		 * for tables and functions.
		 */
		reset_dbentry_counters(dbentry);
	}

	/*
	 * Reset global counters
	 */
	LWLockAcquire(StatsLock, LW_EXCLUSIVE);
	MemSet(shared_globalStats, 0, sizeof(*shared_globalStats));
	MemSet(shared_archiverStats, 0, sizeof(*shared_archiverStats));
	shared_globalStats->stat_reset_timestamp =
		shared_archiverStats->stat_reset_timestamp = GetCurrentTimestamp();
	LWLockRelease(StatsLock);
}

/* ------------------------------------------------------------
 * Public functions used by backends follow
 *------------------------------------------------------------
 */

/* ----------
 * pgstat_flush_stat() -
 *
 *	Must be called by processes that performs DML: tcop/postgres.c, logical
 *	receiver processes, SPI worker, etc. to apply the so far collected
 *	per-table and function usage statistics to the shared statistics hashes.
 *
 *	This requires taking some locks on the shared statistics hashes and some
 *	of updates may be postponed on lock failure. Such postponed updates are
 *	retried in later call of this function and finally cleaned up by calling
 *	this function with force = true or PGSTAT_STAT_MAX_INTERVAL milliseconds
 *	has elapsed since last cleanup. On the other hand updates by regular
 *	backends happen with the interval not shorter than
 *	PGSTAT_STAT_MIN_INTERVAL when force = false.
 *
 *	Returns the time until the next update time in milliseconds.
 *
 *	Note that this is called only out of a transaction, so it is fair to use
 *	transaction stop time as an approximation of current time.
 *	----------
 */
long
pgstat_report_stat(bool force)
{
	static TimestampTz last_flush = 0;
	static TimestampTz pending_since = 0;
	TimestampTz now;
	pgstat_flush_stat_context cxt = {0};
	bool		have_other_stats = false;
	bool		pending_stats = false;
	long		elapsed;
	long		secs;
	int			usecs;

	/* Do we have anything to flush? */
	if (have_recovery_conflicts || n_deadlocks != 0 || n_checksum_failures != 0 || n_tmpfiles != 0)
		have_other_stats = true;

	/* Don't expend a clock check if nothing to do */
	if ((pgStatTabList == NULL || pgStatTabList->tsa_used == 0) &&
		pgStatXactCommit == 0 && pgStatXactRollback == 0 &&
		!have_other_stats && !have_function_stats)
		return 0;

	now = GetCurrentTransactionStopTimestamp();

	if (!force)
	{
		/*
		 * Don't flush stats unless it's been at least
		 * PGSTAT_STAT_MIN_INTERVAL msec since the last flush.  Returns time
		 * to wait in the case.
		 */
		TimestampDifference(last_flush, now, &secs, &usecs);
		elapsed = secs * 1000 + usecs /1000;

		if(elapsed < PGSTAT_STAT_MIN_INTERVAL)
		{
			if (pending_since == 0)
				pending_since = now;

			return PGSTAT_STAT_MIN_INTERVAL - elapsed;
		}


		/*
		 * Don't keep pending stats for longer than PGSTAT_STAT_MAX_INTERVAL.
		 */
		if (pending_since > 0)
		{
			TimestampDifference(pending_since, now, &secs, &usecs);
			elapsed = secs * 1000 + usecs /1000;

			if(elapsed > PGSTAT_STAT_MAX_INTERVAL)
				force = true;
		}
	}

	/* It's the time to flush */
	last_flush = now;

	/* Flush out table stats */
	if (pgStatTabList != NULL && !pgstat_flush_stat(&cxt, !force))
		pending_stats = true;

	/* Flush out function stats */
	if (pgStatFunctions != NULL && !pgstat_flush_funcstats(&cxt, !force))
		pending_stats = true;

	/* Flush out miscellaneous stats */
	if (have_other_stats && !pgstat_flush_miscstats(&cxt, !force))
		pending_stats = true;

	/*  Unpin dbentry if pinned */
	if (cxt.mydb_tabhash)
	{
		dshash_detach(cxt.mydb_tabhash);
		unpin_hashes(cxt.mydbentry, cxt.mygeneration);
		cxt.mydb_tabhash = NULL;
		cxt.mydbentry = NULL;
	}

	/* Publish the last flush time */
	LWLockAcquire(StatsLock, LW_EXCLUSIVE);
	if (StatsShmem->last_update < last_flush)
		StatsShmem->last_update = last_flush;
	LWLockRelease(StatsLock);

	/* record how long we keep pending stats */
	if (pending_stats)
	{
		if (pending_since == 0)
			pending_since = now;
		return PGSTAT_STAT_RETRY_INTERVAL;
	}

	pending_since = 0;

	return 0;
}

/* -------
 * Subroutines for pgstat_flush_stat.
 * -------
 */

/*
 * snapshot_statentry() - Find an entry from source dshash with cache.
 *
 * Returns the entry for key or NULL if not found.
 *
 * Returned entries are consistent during the current transaction or
 * pgstat_clear_snapshot() is called.
 *
 * *cxt->hash points to a HTAB* variable to store the hash for local cache. New
 * one is created if it is not yet created.
 *
 * *cxt->dshash points to dshash_table* variable to store the attached
 * dshash. *cxt->dsh_handle is * attached if not yet attached.
 */
static void *
snapshot_statentry(pgstat_snapshot_cxt *cxt, Oid key)
{
	char *lentry = NULL;
	size_t keysize = cxt->dsh_params->key_size;
	size_t dsh_entrysize = cxt->dsh_params->entry_size;
	bool found;
	bool *negative;

	/* caches the result entry */

	/*
	 * Create new hash with arbitrary initial entries since we don't know how
	 * this hash will grow. The boolean put at the end of the entry is
	 * negative flag.
	 */
	if (!*cxt->hash)
	{
		HASHCTL ctl;

		/* Create the hash in the stats context */
		ctl.keysize		= keysize;
		ctl.entrysize	= cxt->hash_entsize + sizeof(bool);
		ctl.hcxt		= pgStatSnapshotContext;
		*cxt->hash = hash_create(cxt->hash_name, 32, &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	lentry = hash_search(*cxt->hash, &key, HASH_ENTER, &found);

	negative = (bool *) (lentry + cxt->hash_entsize);

	if (!found)
	{
		/* not found in local cache, search shared hash */

		void *sentry;

		/* attach shared hash if not given, leave it alone for later use */
		if (!*cxt->dshash)
		{
			MemoryContext oldcxt;

			if (cxt->dsh_handle == DSM_HANDLE_INVALID)
				return NULL;

			oldcxt = MemoryContextSwitchTo(pgStatSnapshotContext);
			*cxt->dshash =
				dshash_attach(area, cxt->dsh_params, cxt->dsh_handle, NULL);
			MemoryContextSwitchTo(oldcxt);
		}

		sentry = dshash_find(*cxt->dshash, &key, false);

		if (sentry)
		{
			/* found copy it */
			memcpy(lentry, sentry, dsh_entrysize);
			dshash_release_lock(*cxt->dshash, sentry);

			/* then zero out the additional space */
			if (dsh_entrysize < cxt->hash_entsize)
				MemSet(lentry + dsh_entrysize, 0,
					   cxt->hash_entsize - dsh_entrysize);
		}

		*negative = !sentry;
	}

	if (*negative)
		return NULL;

	return (void *) lentry;
}

/*
 * pgstat_flush_stat: Flushes table stats out to shared statistics.
 *
 *  If nowait is true, returns with false if required lock was not acquired
 *  immediately. In the case, infos of some tables may be left alone in TSA to
 *  wait for the next chance. cxt holds some dshash related values that we
 *  want to keep during the shared stats update.  Returns true if no stats
 *  info remains. Caller must detach dshashes stored in cxt after use.
 *
 *  Returns true if all entries are flushed.
 */
static bool
pgstat_flush_stat(pgstat_flush_stat_context *cxt, bool nowait)
{
	static const PgStat_TableCounts all_zeroes;
	TabStatusArray *tsa;
	HTAB		   *new_tsa_hash = NULL;
	TabStatusArray *dest_tsa = pgStatTabList;
	int				dest_elem = 0;
	int				i;

	/* nothing to do, just return  */
	if (pgStatTabHash == NULL)
		return true;

	/*
	 * Destroy pgStatTabHash before we start invalidating PgStat_TableEntry
	 * entries it points to. We recreate it if is needed.
	 */
	hash_destroy(pgStatTabHash);
	pgStatTabHash = NULL;

	/*
	 * Scan through the TabStatusArray struct(s) to find tables that actually
	 * have counts, and try flushing it out to shared statistics.
	 */
	for (tsa = pgStatTabList; tsa != NULL; tsa = tsa->tsa_next)
	{
		for (i = 0; i < tsa->tsa_used; i++)
		{
			PgStat_TableStatus *entry = &tsa->tsa_entries[i];

			/* Shouldn't have any pending transaction-dependent counts */
			Assert(entry->trans == NULL);

			/*
			 * Ignore entries that didn't accumulate any actual counts, such
			 * as indexes that were opened by the planner but not used.
			 */
			if (memcmp(&entry->t_counts, &all_zeroes,
					   sizeof(PgStat_TableCounts)) == 0)
				continue;

			/* try to apply the tab stats */
			if (!pgstat_flush_tabstat(cxt, nowait, entry))
			{
				/*
				 * Failed. Leave it alone filling at the beginning in TSA.
				 */
				TabStatHashEntry *hash_entry;
				bool found;

				if (new_tsa_hash == NULL)
					new_tsa_hash = create_tabstat_hash();

				/* Create hash entry for this entry */
				hash_entry = hash_search(new_tsa_hash, &entry->t_id,
										 HASH_ENTER, &found);
				Assert(!found);

				/*
				 * Move insertion pointer to the next segment. There must be
				 * enough space segments since we are just leaving some of the
				 * current elements.
				 */
				if (dest_elem >= TABSTAT_QUANTUM)
				{
					Assert(dest_tsa->tsa_next != NULL);
					dest_tsa = dest_tsa->tsa_next;
					dest_elem = 0;
				}

				/* Move the entry if needed */
				if (tsa != dest_tsa || i != dest_elem)
				{
					PgStat_TableStatus *new_entry;
					new_entry = &dest_tsa->tsa_entries[dest_elem];
					*new_entry = *entry;
					entry = new_entry;
				}

				hash_entry->tsa_entry = entry;
				dest_elem++;
			}
		}
	}

	/* zero out unused area of TableStatus */
	dest_tsa->tsa_used = dest_elem;
	MemSet(&dest_tsa->tsa_entries[dest_elem], 0,
		   (TABSTAT_QUANTUM - dest_elem) * sizeof(PgStat_TableStatus));
	while (dest_tsa->tsa_next)
	{
		dest_tsa = dest_tsa->tsa_next;
		MemSet(dest_tsa->tsa_entries, 0,
			   dest_tsa->tsa_used * sizeof(PgStat_TableStatus));
		dest_tsa->tsa_used = 0;
	}

	/* and set the new TSA hash if any */
	pgStatTabHash = new_tsa_hash;

	/*
	 * We no longer need shared database and table entries , but still may
	 * use that for my database.
	 */
	if (cxt->shdb_tabhash)
	{
		dshash_detach(cxt->shdb_tabhash);
		unpin_hashes(cxt->shdbentry, cxt->shgeneration);
		cxt->shdb_tabhash = NULL;
		cxt->shdbentry = NULL;
	}

	return pgStatTabHash == NULL;
}


/*
 * pgstat_flush_tabstat: Flushes a table stats entry.
 *
 *  If nowait is true, returns false on lock failure.  Dshashes for table and
 *  function stats are kept attached in ctx. The caller must detach them after
 *  use.
 *
 *  Returns true if the entry is flushed.
 */
bool
pgstat_flush_tabstat(pgstat_flush_stat_context *cxt, bool nowait,
					 PgStat_TableStatus *entry)
{
	Oid		dboid = entry->t_shared ? InvalidOid : MyDatabaseId;
	int		table_mode = PGSTAT_FETCH_EXCLUSIVE;
	bool	updated = false;
	dshash_table *tabhash;
	PgStat_StatDBEntry *dbent;
	int		generation;

	if (nowait)
		table_mode |= PGSTAT_FETCH_NOWAIT;

	/* Attach the required table hash if not yet. */
	if ((entry->t_shared ? cxt->shdb_tabhash : cxt->mydb_tabhash) == NULL)
	{
		/* We don't have corresponding dbentry here */
		dbent = pgstat_get_db_entry(dboid, table_mode, NULL);
		if (!dbent)
			return false;

		/*
		 * We don't hold dshash-lock on dbentries, since the dbentries cannot
		 * be dropped meanwhile.
		 */
		generation = pin_hashes(dbent);
		tabhash = attach_table_hash(dbent, generation);

		if (entry->t_shared)
		{
			cxt->shgeneration = generation;
			cxt->shdbentry = dbent;
			cxt->shdb_tabhash = tabhash;
		}
		else
		{
			cxt->mygeneration = generation;
			cxt->mydbentry = dbent;
			cxt->mydb_tabhash = tabhash;

			/*
			 * We attach mydb tabhash once per flushing. This is the chance to
			 * update database-wide stats
			 */
			LWLockAcquire(&dbent->lock, LW_EXCLUSIVE);
			dbent->n_xact_commit += pgStatXactCommit;
			dbent->n_xact_rollback += pgStatXactRollback;
			dbent->n_block_read_time += pgStatBlockReadTime;
			dbent->n_block_write_time += pgStatBlockWriteTime;
			LWLockRelease(&dbent->lock);
			pgStatXactCommit = 0;
			pgStatXactRollback = 0;
			pgStatBlockReadTime = 0;
			pgStatBlockWriteTime = 0;
		}
	}
	else if (entry->t_shared)
	{
		dbent = cxt->shdbentry;
		tabhash = cxt->shdb_tabhash;
	}
	else
	{
		dbent = cxt->mydbentry;
		tabhash = cxt->mydb_tabhash;
	}


	/*
	 * dbentry is always available here, so try flush table stats first, then
	 * database stats.
	 */
	if (pgstat_update_tabentry(tabhash, entry, nowait))
	{
		pgstat_update_dbentry(dbent, entry);
		updated = true;
	}

	return updated;
}

/*
 * pgstat_flush_funcstats: Flushes function stats.
 *
 *  If nowait is true, returns false on lock failure and leave some of the
 *  entries alone in the local hash.
 *
 *  Returns true if all entries are flushed.
 */
static bool
pgstat_flush_funcstats(pgstat_flush_stat_context *cxt, bool nowait)
{
	/* we assume this inits to all zeroes: */
	static const PgStat_FunctionCounts all_zeroes;
	dshash_table   *funchash;
	HASH_SEQ_STATUS fstat;
	PgStat_BackendFunctionEntry *bestat;

	/* nothing to do, just return  */
	if (pgStatFunctions == NULL)
		return true;

	/* get dbentry into cxt if not yet.  */
	if (cxt->mydbentry == NULL)
	{
		int op = PGSTAT_FETCH_EXCLUSIVE;

		if (nowait)
			op |= PGSTAT_FETCH_NOWAIT;

		cxt->mydbentry = pgstat_get_db_entry(MyDatabaseId, op, NULL);

		if (cxt->mydbentry == NULL)
			return false;

		cxt->mygeneration = pin_hashes(cxt->mydbentry);
	}

	funchash = attach_function_hash(cxt->mydbentry, cxt->mygeneration);
	if (funchash == NULL)
		return false;

	have_function_stats = false;

	/*
	 * Scan through the pgStatFunctions to find functions that actually have
	 * counts, and try flushing it out to shared statistics.
	 */
	hash_seq_init(&fstat, pgStatFunctions);
	while ((bestat = (PgStat_BackendFunctionEntry *) hash_seq_search(&fstat)) != NULL)
	{
		bool found;
		PgStat_StatFuncEntry *funcent = NULL;

		/* Skip it if no counts accumulated for it so far */
		if (memcmp(&bestat->f_counts, &all_zeroes,
				   sizeof(PgStat_FunctionCounts)) == 0)
			continue;

		funcent = (PgStat_StatFuncEntry *)
			dshash_find_or_insert_extended(funchash, (void *) &(bestat->f_id),
										   &found, nowait);

		/*
		 * We couldn't acquire lock on the required entry. Leave the local
		 * entry alone.
		 */
		if (!funcent)
		{
			have_function_stats = true;
			continue;
		}

		/* Initialize if it's new, or add to it. */
		if (!found)
		{
			funcent->functionid = bestat->f_id;
			funcent->f_numcalls = bestat->f_counts.f_numcalls;
			funcent->f_total_time =
				INSTR_TIME_GET_MICROSEC(bestat->f_counts.f_total_time);
			funcent->f_self_time =
				INSTR_TIME_GET_MICROSEC(bestat->f_counts.f_self_time);
		}
		else
		{
			funcent->f_numcalls += bestat->f_counts.f_numcalls;
			funcent->f_total_time +=
				INSTR_TIME_GET_MICROSEC(bestat->f_counts.f_total_time);
			funcent->f_self_time +=
				INSTR_TIME_GET_MICROSEC(bestat->f_counts.f_self_time);
		}
		dshash_release_lock(funchash, funcent);

		/* reset used counts */
		MemSet(&bestat->f_counts, 0, sizeof(PgStat_FunctionCounts));
	}

	return !have_function_stats;
}

/*
 * pgstat_flush_miscstats: Flushes out miscellaneous stats.
 *
 *  If nowait is true, returns with false on lock failure on dbentry.
 *
 *  Returns true if all the miscellaneous stats are flushed out.
 */
static bool
pgstat_flush_miscstats(pgstat_flush_stat_context *cxt, bool nowait)
{
	/* get dbentry if not yet.  */
	if (cxt->mydbentry == NULL)
	{
		int op = PGSTAT_FETCH_EXCLUSIVE;
		if (nowait)
			op |= PGSTAT_FETCH_NOWAIT;

		cxt->mydbentry = pgstat_get_db_entry(MyDatabaseId, op, NULL);

		/* Lock failure, return. */
		if (cxt->mydbentry == NULL)
			return false;

		cxt->mygeneration = pin_hashes(cxt->mydbentry);
	}

	LWLockAcquire(&cxt->mydbentry->lock, LW_EXCLUSIVE);
	if (have_recovery_conflicts)
		pgstat_flush_recovery_conflict(cxt->mydbentry);
	if (n_deadlocks != 0)
		pgstat_flush_deadlock(cxt->mydbentry);
	if (n_checksum_failures != 0)
		pgstat_flush_checksum_failures(cxt->mydbentry);
	if (n_tmpfiles != 0)
		pgstat_flush_tempfile(cxt->mydbentry);
	LWLockRelease(&cxt->mydbentry->lock);

	return true;
}

/* ----------
 * pgstat_vacuum_stat() -
 *
 *	Remove objects he can get rid of.
 * ----------
 */
void
pgstat_vacuum_stat(void)
{
	HTAB	   *oidtab;
	dshash_table *dshtable;
	dshash_seq_status dshstat;
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;
	PgStat_StatFuncEntry *funcentry;

	/* we don't collect statistics under standalone mode */
	if (!IsUnderPostmaster)
		return;

	/* If not done for this transaction, take a snapshot of stats */
	pgstat_snapshot_global_stats();

	/*
	 * Read pg_database and make a list of OIDs of all existing databases
	 */
	oidtab = pgstat_collect_oids(DatabaseRelationId, Anum_pg_database_oid);

	/*
	 * Search the database hash table for dead databases and drop them
	 * from the hash.
	 */

	dshash_seq_init(&dshstat, pgStatDBHash, false, true);
	while ((dbentry = (PgStat_StatDBEntry *) dshash_seq_next(&dshstat)) != NULL)
	{
		Oid			dbid = dbentry->databaseid;

		CHECK_FOR_INTERRUPTS();

		/* the DB entry for shared tables (with InvalidOid) is never dropped */
		if (OidIsValid(dbid) &&
			hash_search(oidtab, (void *) &dbid, HASH_FIND, NULL) == NULL)
			pgstat_drop_database(dbid);
	}

	/* Clean up */
	hash_destroy(oidtab);

	/*
	 * Lookup our own database entry; if not found, nothing more to do.
	 */
	dbentry = pgstat_get_db_entry(MyDatabaseId, PGSTAT_FETCH_EXCLUSIVE, NULL);
	if (!dbentry)
		return;

	/*
	 * Similarly to above, make a list of all known relations in this DB.
	 */
	oidtab = pgstat_collect_oids(RelationRelationId, Anum_pg_class_oid);

	/*
	 * Check for all tables listed in stats hashtable if they still exist.
	 * Stats cache is useless here so directly search the shared hash.
	 */
	dshtable = dshash_attach(area, &dsh_tblparams, dbentry->tables, 0);
	dshash_seq_init(&dshstat, dshtable, false, true);
	while ((tabentry = (PgStat_StatTabEntry *) dshash_seq_next(&dshstat)) != NULL)
	{
		Oid			tabid = tabentry->tableid;

		CHECK_FOR_INTERRUPTS();

		if (hash_search(oidtab, (void *) &tabid, HASH_FIND, NULL) != NULL)
			continue;

		/* Not there, so purge this table */
		dshash_delete_entry(dshtable, tabentry);
	}
	dshash_detach(dshtable);

	/* Clean up */
	hash_destroy(oidtab);

	/*
	 * Now repeat the above steps for functions.  However, we needn't bother
	 * in the common case where no function stats are being collected.
	 */
	if (dbentry->functions != DSM_HANDLE_INVALID)
	{
		dshtable =
			dshash_attach(area, &dsh_funcparams, dbentry->functions, 0);
		oidtab = pgstat_collect_oids(ProcedureRelationId, Anum_pg_proc_oid);

		dshash_seq_init(&dshstat, dshtable, false, true);
		while ((funcentry = (PgStat_StatFuncEntry *) dshash_seq_next(&dshstat)) != NULL)
		{
			Oid			funcid = funcentry->functionid;

			CHECK_FOR_INTERRUPTS();

			if (hash_search(oidtab, (void *) &funcid, HASH_FIND, NULL) != NULL)
				continue;

			/* Not there, so remove this function */
			dshash_delete_entry(dshtable, funcentry);
		}

		hash_destroy(oidtab);

		dshash_detach(dshtable);
	}
	dshash_release_lock(pgStatDBHash, dbentry);
}


/* ----------
 * pgstat_collect_oids() -
 *
 *	Collect the OIDs of all objects listed in the specified system catalog
 *	into a temporary hash table.  Caller should hash_destroy the result
 *	when done with it.  (However, we make the table in CurrentMemoryContext
 *	so that it will be freed properly in event of an error.)
 * ----------
 */
static HTAB *
pgstat_collect_oids(Oid catalogid, AttrNumber anum_oid)
{
	HTAB	   *htab;
	HASHCTL		hash_ctl;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	Snapshot	snapshot;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(Oid);
	hash_ctl.hcxt = CurrentMemoryContext;
	htab = hash_create("Temporary table of OIDs",
					   PGSTAT_TAB_HASH_SIZE,
					   &hash_ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	rel = table_open(catalogid, AccessShareLock);
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan(rel, snapshot, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			thisoid;
		bool		isnull;

		thisoid = heap_getattr(tup, anum_oid, RelationGetDescr(rel), &isnull);
		Assert(!isnull);

		CHECK_FOR_INTERRUPTS();

		(void) hash_search(htab, (void *) &thisoid, HASH_ENTER, NULL);
	}
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(rel, AccessShareLock);

	return htab;
}


/* ----------
 * pgstat_drop_database() -
 *
 *	Remove entry for the database that we just dropped.
 *
 *	If some stats are flushed after this, this entry will re-created but we
 *	will still clean the dead DB eventually via future invocations of
 *	pgstat_vacuum_stat().
 * ----------
 */
void
pgstat_drop_database(Oid databaseid)
{
	PgStat_StatDBEntry *dbentry;

	Assert (OidIsValid(databaseid));
	Assert(pgStatDBHash);

	/*
	 * Lookup the database in the hashtable with exclusive lock.
	 */
	dbentry = pgstat_get_db_entry(databaseid, PGSTAT_FETCH_EXCLUSIVE, NULL);

	/*
	 * If found, remove it (along with the db statfile).
	 */
	if (dbentry)
	{
		LWLockAcquire(&dbentry->lock, LW_EXCLUSIVE);
		Assert(dbentry->refcnt == 0);

		/* One one must live on this database. It's safe to drop all. */
		if (dbentry->tables != DSM_HANDLE_INVALID)
		{
			dshash_table *tbl =
				dshash_attach(area, &dsh_tblparams, dbentry->tables, 0);
			dshash_destroy(tbl);
		}
		if (dbentry->functions != DSM_HANDLE_INVALID)
		{
			dshash_table *tbl =
				dshash_attach(area, &dsh_funcparams, dbentry->functions, 0);
			dshash_destroy(tbl);
		}
		LWLockRelease(&dbentry->lock);

		dshash_delete_entry(pgStatDBHash, (void *)dbentry);
	}
}

/* ----------
 * pgstat_reset_counters() -
 *
 *	Reset counters for our database.
 *
 *	Permission checking for this function is managed through the normal
 *	GRANT system.
 * ----------
 */
void
pgstat_reset_counters(void)
{
	PgStat_StatDBEntry	   *dbentry;
	PgStat_TableLookupState status;

	Assert(pgStatDBHash);

	/*
	 * Lookup the database in the hashtable.  Nothing to do if not there.
	 */
	dbentry = pgstat_get_db_entry(MyDatabaseId, PGSTAT_FETCH_EXCLUSIVE, &status);

	if (!dbentry)
		return;

	/* This database is active, safe to release the lock immediately. */
	dshash_release_lock(pgStatDBHash, dbentry);

	/* Reset database-level stats. */
	reset_dbentry_counters(dbentry);

}

/* ----------
 * pgstat_reset_shared_counters() -
 *
 *	Reset cluster-wide shared counters.
 *
 *	Permission checking for this function is managed through the normal
 *	GRANT system.
 * ----------
 */
void
pgstat_reset_shared_counters(const char *target)
{
	/* Reset the archiver statistics for the cluster. */
	if (strcmp(target, "archiver") == 0)
	{
		LWLockAcquire(StatsLock, LW_EXCLUSIVE);

		MemSet(shared_archiverStats, 0, sizeof(*shared_archiverStats));
		shared_archiverStats->stat_reset_timestamp = GetCurrentTimestamp();
	}
	else if (strcmp(target, "bgwriter") == 0)
	{
		LWLockAcquire(StatsLock, LW_EXCLUSIVE);

		/* Reset the global background writer statistics for the cluster. */
		MemSet(shared_globalStats, 0, sizeof(*shared_globalStats));
		shared_globalStats->stat_reset_timestamp = GetCurrentTimestamp();
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized reset target: \"%s\"", target),
				 errhint("Target must be \"archiver\" or \"bgwriter\".")));

	LWLockRelease(StatsLock);
}

/* ----------
 * pgstat_reset_single_counter() -
 *
 *	Reset a single counter.
 *
 *	Permission checking for this function is managed through the normal
 *	GRANT system.
 * ----------
 */
void
pgstat_reset_single_counter(Oid objoid, PgStat_Single_Reset_Type type)
{
	PgStat_StatDBEntry *dbentry;
	TimestampTz ts;
	int generation;

	dbentry = pgstat_get_db_entry(MyDatabaseId, PGSTAT_FETCH_EXCLUSIVE, NULL);

	if (!dbentry)
		return;

	/* This database is active, safe to release the lock immediately. */
	generation = pin_hashes(dbentry);

	/* Set the reset timestamp for the whole database */
	ts = GetCurrentTimestamp();
	LWLockAcquire(&dbentry->lock, LW_EXCLUSIVE);
	dbentry->stat_reset_timestamp = ts;
	LWLockRelease(&dbentry->lock);

	/* Remove object if it exists, ignore if not */
	if (type == RESET_TABLE)
	{
		dshash_table *t = attach_table_hash(dbentry, generation);
		dshash_delete_key(t, (void *) &objoid);
		dshash_detach(t);
	}

	if (type == RESET_FUNCTION)
	{
		dshash_table *t = attach_function_hash(dbentry, generation);
		if (t)
		{
			dshash_delete_key(t, (void *) &objoid);
			dshash_detach(t);
		}
	}
	unpin_hashes(dbentry, generation);
}

/* ----------
 * pgstat_report_autovac() -
 *
 *	Called from autovacuum.c to report startup of an autovacuum process.
 *	We are called before InitPostgres is done, so can't rely on MyDatabaseId;
 *	the db OID must be passed in, instead.
 * ----------
 */
void
pgstat_report_autovac(Oid dboid)
{
	PgStat_StatDBEntry *dbentry;
	TimestampTz ts;

	Assert(pgStatDBHash);

	if (!pgstat_track_counts || !IsUnderPostmaster)
		return;

	/*
	 * Store the last autovacuum time in the database's hashtable entry.
	 */
	dbentry = pgstat_get_db_entry(dboid, PGSTAT_FETCH_EXCLUSIVE, NULL);
	dshash_release_lock(pgStatDBHash, dbentry);

	ts = GetCurrentTimestamp();

	LWLockAcquire(&dbentry->lock, LW_EXCLUSIVE);
	dbentry->last_autovac_time = ts;
	LWLockRelease(&dbentry->lock);
}


/* ---------
 * pgstat_report_vacuum() -
 *
 *	Report about the table we just vacuumed.
 * ---------
 */
void
pgstat_report_vacuum(Oid tableoid, bool shared,
					 PgStat_Counter livetuples, PgStat_Counter deadtuples)
{
	Oid					dboid;
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;
	dshash_table *table;
	int					generation;

	Assert(pgStatDBHash);

	if (!pgstat_track_counts || !IsUnderPostmaster)
		return;

	dboid = shared ? InvalidOid : MyDatabaseId;

	/*
	 * Store the data in the table's hash table entry.
	 */
	dbentry = pgstat_get_db_entry(dboid, PGSTAT_FETCH_EXCLUSIVE, NULL);
	generation = pin_hashes(dbentry);
	table = attach_table_hash(dbentry, generation);

	tabentry = pgstat_get_tab_entry(table, tableoid, true);

	tabentry->n_live_tuples = livetuples;
	tabentry->n_dead_tuples = deadtuples;

	if (IsAutoVacuumWorkerProcess())
	{
		tabentry->autovac_vacuum_timestamp = GetCurrentTimestamp();
		tabentry->autovac_vacuum_count++;
	}
	else
	{
		tabentry->vacuum_timestamp = GetCurrentTimestamp();
		tabentry->vacuum_count++;
	}
	dshash_release_lock(table, tabentry);

	dshash_detach(table);
	unpin_hashes(dbentry, generation);
}

/* --------
 * pgstat_report_analyze() -
 *
 *	Report about the table we just analyzed.
 *
 * Caller must provide new live- and dead-tuples estimates, as well as a
 * flag indicating whether to reset the changes_since_analyze counter.
 * --------
 */
void
pgstat_report_analyze(Relation rel,
					  PgStat_Counter livetuples, PgStat_Counter deadtuples,
					  bool resetcounter)
{
	Oid					dboid;
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;
	dshash_table	   *table;
	int					generation;

	Assert(pgStatDBHash);

	if (!pgstat_track_counts || !IsUnderPostmaster)
		return;

	/*
	 * Unlike VACUUM, ANALYZE might be running inside a transaction that has
	 * already inserted and/or deleted rows in the target table. ANALYZE will
	 * have counted such rows as live or dead respectively. Because we will
	 * report our counts of such rows at transaction end, we should subtract
	 * off these counts from what we send to the collector now, else they'll
	 * be double-counted after commit.  (This approach also ensures that the
	 * collector ends up with the right numbers if we abort instead of
	 * committing.)
	 */
	if (rel->pgstat_info != NULL)
	{
		PgStat_TableXactStatus *trans;

		for (trans = rel->pgstat_info->trans; trans; trans = trans->upper)
		{
			livetuples -= trans->tuples_inserted - trans->tuples_deleted;
			deadtuples -= trans->tuples_updated + trans->tuples_deleted;
		}
		/* count stuff inserted by already-aborted subxacts, too */
		deadtuples -= rel->pgstat_info->t_counts.t_delta_dead_tuples;
		/* Since ANALYZE's counts are estimates, we could have underflowed */
		livetuples = Max(livetuples, 0);
		deadtuples = Max(deadtuples, 0);
	}

	dboid = rel->rd_rel->relisshared ? InvalidOid : MyDatabaseId;

	/*
	 * Store the data in the table's hashtable entry.
	 */
	dbentry = pgstat_get_db_entry(dboid, PGSTAT_FETCH_EXCLUSIVE, NULL);
	generation = pin_hashes(dbentry);
 	table = attach_table_hash(dbentry, generation);
	tabentry = pgstat_get_tab_entry(table, RelationGetRelid(rel), true);

	tabentry->n_live_tuples = livetuples;
	tabentry->n_dead_tuples = deadtuples;

	/*
	 * If commanded, reset changes_since_analyze to zero.  This forgets any
	 * changes that were committed while the ANALYZE was in progress, but we
	 * have no good way to estimate how many of those there were.
	 */
	if (resetcounter)
		tabentry->changes_since_analyze = 0;

	if (IsAutoVacuumWorkerProcess())
	{
		tabentry->autovac_analyze_timestamp = GetCurrentTimestamp();
		tabentry->autovac_analyze_count++;
	}
	else
	{
		tabentry->analyze_timestamp = GetCurrentTimestamp();
		tabentry->analyze_count++;
	}
	dshash_release_lock(table, tabentry);

	dshash_detach(table);
	unpin_hashes(dbentry, generation);
}

/* --------
 * pgstat_report_recovery_conflict() -
 *
 *	Report a Hot Standby recovery conflict.
 * --------
 */
void
pgstat_report_recovery_conflict(int reason)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_TableLookupState status;

	Assert(pgStatDBHash);

	if (!pgstat_track_counts || !IsUnderPostmaster)
		return;

	have_recovery_conflicts = true;

	switch (reason)
	{
		case PROCSIG_RECOVERY_CONFLICT_DATABASE:

			/*
			 * Since we drop the information about the database as soon as it
			 * replicates, there is no point in counting these conflicts.
			 */
			break;
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			n_conflict_tablespace++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOCK:
			n_conflict_lock++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:
			n_conflict_snapshot++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:
			n_conflict_bufferpin++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:
			n_conflict_startup_deadlock++;
			break;
	}

	dbentry = pgstat_get_db_entry(MyDatabaseId,
								  PGSTAT_FETCH_EXCLUSIVE | PGSTAT_FETCH_NOWAIT,
								  &status);

	if (status == PGSTAT_ENTRY_LOCK_FAILED)
		return;

	/* We had a chance to flush immediately */
	pgstat_flush_recovery_conflict(dbentry);

	dshash_release_lock(pgStatDBHash, dbentry);
}

/*
 * flush recovery conflict stats
 */
static void
pgstat_flush_recovery_conflict(PgStat_StatDBEntry *dbentry)
{
	dbentry->n_conflict_tablespace	+= n_conflict_tablespace;
	dbentry->n_conflict_lock 		+= n_conflict_lock;
	dbentry->n_conflict_snapshot	+= n_conflict_snapshot;
	dbentry->n_conflict_bufferpin	+= n_conflict_bufferpin;
	dbentry->n_conflict_startup_deadlock += n_conflict_startup_deadlock;

	n_conflict_tablespace = 0;
	n_conflict_lock = 0;
	n_conflict_snapshot = 0;
	n_conflict_bufferpin = 0;
	n_conflict_startup_deadlock = 0;

	have_recovery_conflicts = false;
}

/* --------
 * pgstat_report_deadlock() -
 *
 *	Report a deadlock detected.
 * --------
 */
void
pgstat_report_deadlock(void)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_TableLookupState status;

	Assert(pgStatDBHash);

	if (!pgstat_track_counts || !IsUnderPostmaster)
		return;

	n_deadlocks++;

	dbentry = pgstat_get_db_entry(MyDatabaseId,
								  PGSTAT_FETCH_EXCLUSIVE | PGSTAT_FETCH_NOWAIT,
								  &status);

	if (status == PGSTAT_ENTRY_LOCK_FAILED)
		return;

	/* We had a chance to flush immediately */
	pgstat_flush_deadlock(dbentry);

	dshash_release_lock(pgStatDBHash, dbentry);
}

/*
 * flush dead lock stats
 */
static void
pgstat_flush_deadlock(PgStat_StatDBEntry *dbentry)
{
	dbentry->n_deadlocks += n_deadlocks;
	n_deadlocks = 0;
}

/* --------
 * pgstat_report_checksum_failures_in_db(dboid, failure_count) -
 *
 *  Report one or more checksum failures.
 * --------
 */
void
pgstat_report_checksum_failures_in_db(Oid dboid, int failurecount)
{
	PgStat_StatDBEntry *dbentry;

	Assert(pgStatDBHash);

	if (!pgstat_track_counts || !IsUnderPostmaster)
		return;

	dbentry = pgstat_get_db_entry(dboid, PGSTAT_FETCH_EXCLUSIVE, NULL);
	dbentry->n_checksum_failures += failurecount;

	dshash_release_lock(pgStatDBHash, dbentry);
}

/* --------
 * pgstat_report_checksum_failure() -
 *
 *	Report a checksum failure in the current database.
 * --------
 */
void
pgstat_report_checksum_failure(void)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_TableLookupState status;

	Assert(pgStatDBHash);

	if (!pgstat_track_counts || !IsUnderPostmaster)
		return;

	n_checksum_failures++;

	dbentry = pgstat_get_db_entry(MyDatabaseId,
								  PGSTAT_FETCH_EXCLUSIVE | PGSTAT_FETCH_NOWAIT,
								  &status);

	if (status == PGSTAT_ENTRY_LOCK_FAILED)
		return;

	/* We had a chance to flush immediately */
	pgstat_flush_checksum_failures(dbentry);

	dshash_release_lock(pgStatDBHash, dbentry);
}

/*
 * flush checksum failures
 */
static void
pgstat_flush_checksum_failures(PgStat_StatDBEntry *dbentry)
{
	dbentry->n_checksum_failures += n_checksum_failures;
	n_checksum_failures = 0;
}

/* --------
 * pgstat_report_tempfile() -
 *
 *	Report a temporary file.
 * --------
 */
void
pgstat_report_tempfile(size_t filesize)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_TableLookupState status;

	Assert(pgStatDBHash);

	if (!pgstat_track_counts || !IsUnderPostmaster)
		return;

	if (filesize > 0) /* Is there a case where filesize is really 0? */
	{
		n_tmpfilesize += filesize; /* needs check overflow */
		n_tmpfiles++;
	}

	if (n_tmpfiles == 0)
		return;

	dbentry = pgstat_get_db_entry(MyDatabaseId,
								  PGSTAT_FETCH_EXCLUSIVE | PGSTAT_FETCH_NOWAIT,
								  &status);

	if (status == PGSTAT_ENTRY_LOCK_FAILED)
		return;

	/* We had a chance to flush immediately */
	pgstat_flush_tempfile(dbentry);

	dshash_release_lock(pgStatDBHash, dbentry);
}

/*
 * flush temporary file stats
 */
static void
pgstat_flush_tempfile(PgStat_StatDBEntry *dbentry)
{
	dbentry->n_temp_bytes += n_tmpfilesize;
	dbentry->n_temp_files += n_tmpfiles;
	n_tmpfilesize = 0;
	n_tmpfiles = 0;
}

/*
 * Initialize function call usage data.
 * Called by the executor before invoking a function.
 */
void
pgstat_init_function_usage(FunctionCallInfo fcinfo,
						   PgStat_FunctionCallUsage *fcu)
{
	PgStat_BackendFunctionEntry *htabent;
	bool		found;

	if (pgstat_track_functions <= fcinfo->flinfo->fn_stats)
	{
		/* stats not wanted */
		fcu->fs = NULL;
		return;
	}

	if (!pgStatFunctions)
	{
		/* First time through - initialize function stat table */
		HASHCTL		hash_ctl;

		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(PgStat_BackendFunctionEntry);
		pgStatFunctions = hash_create("Function stat entries",
									  PGSTAT_FUNCTION_HASH_SIZE,
									  &hash_ctl,
									  HASH_ELEM | HASH_BLOBS);
	}

	/* Get the stats entry for this function, create if necessary */
	htabent = hash_search(pgStatFunctions, &fcinfo->flinfo->fn_oid,
						  HASH_ENTER, &found);
	if (!found)
		MemSet(&htabent->f_counts, 0, sizeof(PgStat_FunctionCounts));

	fcu->fs = &htabent->f_counts;

	/* save stats for this function, later used to compensate for recursion */
	fcu->save_f_total_time = htabent->f_counts.f_total_time;

	/* save current backend-wide total time */
	fcu->save_total = total_func_time;

	/* get clock time as of function start */
	INSTR_TIME_SET_CURRENT(fcu->f_start);
}

/*
 * find_funcstat_entry - find any existing PgStat_BackendFunctionEntry entry
 *		for specified function
 *
 * If no entry, return NULL, don't create a new one
 */
PgStat_BackendFunctionEntry *
find_funcstat_entry(Oid func_id)
{
	if (pgStatFunctions == NULL)
		return NULL;

	return (PgStat_BackendFunctionEntry *) hash_search(pgStatFunctions,
													   (void *) &func_id,
													   HASH_FIND, NULL);
}

/*
 * Calculate function call usage and update stat counters.
 * Called by the executor after invoking a function.
 *
 * In the case of a set-returning function that runs in value-per-call mode,
 * we will see multiple pgstat_init_function_usage/pgstat_end_function_usage
 * calls for what the user considers a single call of the function.  The
 * finalize flag should be TRUE on the last call.
 */
void
pgstat_end_function_usage(PgStat_FunctionCallUsage *fcu, bool finalize)
{
	PgStat_FunctionCounts *fs = fcu->fs;
	instr_time	f_total;
	instr_time	f_others;
	instr_time	f_self;

	/* stats not wanted? */
	if (fs == NULL)
		return;

	/* total elapsed time in this function call */
	INSTR_TIME_SET_CURRENT(f_total);
	INSTR_TIME_SUBTRACT(f_total, fcu->f_start);

	/* self usage: elapsed minus anything already charged to other calls */
	f_others = total_func_time;
	INSTR_TIME_SUBTRACT(f_others, fcu->save_total);
	f_self = f_total;
	INSTR_TIME_SUBTRACT(f_self, f_others);

	/* update backend-wide total time */
	INSTR_TIME_ADD(total_func_time, f_self);

	/*
	 * Compute the new f_total_time as the total elapsed time added to the
	 * pre-call value of f_total_time.  This is necessary to avoid
	 * double-counting any time taken by recursive calls of myself.  (We do
	 * not need any similar kluge for self time, since that already excludes
	 * any recursive calls.)
	 */
	INSTR_TIME_ADD(f_total, fcu->save_f_total_time);

	/* update counters in function stats table */
	if (finalize)
		fs->f_numcalls++;
	fs->f_total_time = f_total;
	INSTR_TIME_ADD(fs->f_self_time, f_self);

	/* indicate that we have something to send */
	have_function_stats = true;
}


/* ----------
 * pgstat_initstats() -
 *
 *	Initialize a relcache entry to count access statistics.
 *	Called whenever a relation is opened.
 *
 *	We assume that a relcache entry's pgstat_info field is zeroed by
 *	relcache.c when the relcache entry is made; thereafter it is long-lived
 *	data.  We can avoid repeated searches of the TabStatus arrays when the
 *	same relation is touched repeatedly within a transaction.
 * ----------
 */
void
pgstat_initstats(Relation rel)
{
	Oid			rel_id = rel->rd_id;
	char		relkind = rel->rd_rel->relkind;

	/* We only count stats for things that have storage */
	if (!(relkind == RELKIND_RELATION ||
		  relkind == RELKIND_MATVIEW ||
		  relkind == RELKIND_INDEX ||
		  relkind == RELKIND_TOASTVALUE ||
		  relkind == RELKIND_SEQUENCE))
	{
		rel->pgstat_info = NULL;
		return;
	}

	if (!pgstat_track_counts || !IsUnderPostmaster)
	{
		/* We're not counting at all */
		rel->pgstat_info = NULL;
		return;
	}

	/*
	 * If we already set up this relation in the current transaction, nothing
	 * to do.
	 */
	if (rel->pgstat_info != NULL &&
		rel->pgstat_info->t_id == rel_id)
		return;

	/* Else find or make the PgStat_TableStatus entry, and update link */
	rel->pgstat_info = get_tabstat_entry(rel_id, rel->rd_rel->relisshared);
}

/*
 * create_tabstat_hash - create local hash as transactional storage
 */
static HTAB *
create_tabstat_hash(void)
{
	HASHCTL		ctl;

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(TabStatHashEntry);

	return hash_create("pgstat TabStatusArray lookup hash table",
					   TABSTAT_QUANTUM,
					   &ctl,
					   HASH_ELEM | HASH_BLOBS);
}

/*
 * get_tabstat_entry - find or create a PgStat_TableStatus entry for rel
 */
static PgStat_TableStatus *
get_tabstat_entry(Oid rel_id, bool isshared)
{
	TabStatHashEntry *hash_entry;
	PgStat_TableStatus *entry;
	TabStatusArray *tsa;
	bool		found;

	/*
	 * Create hash table if we don't have it already.
	 */
	if (pgStatTabHash == NULL)
		pgStatTabHash = create_tabstat_hash();

	/*
	 * Find an entry or create a new one.
	 */
	hash_entry = hash_search(pgStatTabHash, &rel_id, HASH_ENTER, &found);
	if (!found)
	{
		/* initialize new entry with null pointer */
		hash_entry->tsa_entry = NULL;
	}

	/*
	 * If entry is already valid, we're done.
	 */
	if (hash_entry->tsa_entry)
		return hash_entry->tsa_entry;

	/*
	 * Locate the first pgStatTabList entry with free space, making a new list
	 * entry if needed.  Note that we could get an OOM failure here, but if so
	 * we have left the hashtable and the list in a consistent state.
	 */
	if (pgStatTabList == NULL)
	{
		/* Set up first pgStatTabList entry */
		pgStatTabList = (TabStatusArray *)
			MemoryContextAllocZero(TopMemoryContext,
								   sizeof(TabStatusArray));
	}

	tsa = pgStatTabList;
	while (tsa->tsa_used >= TABSTAT_QUANTUM)
	{
		if (tsa->tsa_next == NULL)
			tsa->tsa_next = (TabStatusArray *)
				MemoryContextAllocZero(TopMemoryContext,
									   sizeof(TabStatusArray));
		tsa = tsa->tsa_next;
	}

	/*
	 * Allocate a PgStat_TableStatus entry within this list entry.  We assume
	 * the entry was already zeroed, either at creation or after last use.
	 */
	entry = &tsa->tsa_entries[tsa->tsa_used++];
	entry->t_id = rel_id;
	entry->t_shared = isshared;

	/*
	 * Now we can fill the entry in pgStatTabHash.
	 */
	hash_entry->tsa_entry = entry;

	return entry;
}

/*
 * find_tabstat_entry - find any existing PgStat_TableStatus entry for rel
 *
 * If no entry, return NULL, don't create a new one
 *
 * Note: if we got an error in the most recent execution of pgstat_report_stat,
 * it's possible that an entry exists but there's no hashtable entry for it.
 * That's okay, we'll treat this case as "doesn't exist".
 */
PgStat_TableStatus *
find_tabstat_entry(Oid rel_id)
{
	TabStatHashEntry *hash_entry;

	/* If hashtable doesn't exist, there are no entries at all */
	if (!pgStatTabHash)
		return NULL;

	hash_entry = hash_search(pgStatTabHash, &rel_id, HASH_FIND, NULL);
	if (!hash_entry)
		return NULL;

	/* Note that this step could also return NULL, but that's correct */
	return hash_entry->tsa_entry;
}

/*
 * get_tabstat_stack_level - add a new (sub)transaction stack entry if needed
 */
static PgStat_SubXactStatus *
get_tabstat_stack_level(int nest_level)
{
	PgStat_SubXactStatus *xact_state;

	xact_state = pgStatXactStack;
	if (xact_state == NULL || xact_state->nest_level != nest_level)
	{
		xact_state = (PgStat_SubXactStatus *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(PgStat_SubXactStatus));
		xact_state->nest_level = nest_level;
		xact_state->prev = pgStatXactStack;
		xact_state->first = NULL;
		pgStatXactStack = xact_state;
	}
	return xact_state;
}

/*
 * add_tabstat_xact_level - add a new (sub)transaction state record
 */
static void
add_tabstat_xact_level(PgStat_TableStatus *pgstat_info, int nest_level)
{
	PgStat_SubXactStatus *xact_state;
	PgStat_TableXactStatus *trans;

	/*
	 * If this is the first rel to be modified at the current nest level, we
	 * first have to push a transaction stack entry.
	 */
	xact_state = get_tabstat_stack_level(nest_level);

	/* Now make a per-table stack entry */
	trans = (PgStat_TableXactStatus *)
		MemoryContextAllocZero(TopTransactionContext,
							   sizeof(PgStat_TableXactStatus));
	trans->nest_level = nest_level;
	trans->upper = pgstat_info->trans;
	trans->parent = pgstat_info;
	trans->next = xact_state->first;
	xact_state->first = trans;
	pgstat_info->trans = trans;
}

/*
 * pgstat_count_heap_insert - count a tuple insertion of n tuples
 */
void
pgstat_count_heap_insert(Relation rel, PgStat_Counter n)
{
	PgStat_TableStatus *pgstat_info = rel->pgstat_info;

	if (pgstat_info != NULL)
	{
		/* We have to log the effect at the proper transactional level */
		int			nest_level = GetCurrentTransactionNestLevel();

		if (pgstat_info->trans == NULL ||
			pgstat_info->trans->nest_level != nest_level)
			add_tabstat_xact_level(pgstat_info, nest_level);

		pgstat_info->trans->tuples_inserted += n;
	}
}

/*
 * pgstat_count_heap_update - count a tuple update
 */
void
pgstat_count_heap_update(Relation rel, bool hot)
{
	PgStat_TableStatus *pgstat_info = rel->pgstat_info;

	if (pgstat_info != NULL)
	{
		/* We have to log the effect at the proper transactional level */
		int			nest_level = GetCurrentTransactionNestLevel();

		if (pgstat_info->trans == NULL ||
			pgstat_info->trans->nest_level != nest_level)
			add_tabstat_xact_level(pgstat_info, nest_level);

		pgstat_info->trans->tuples_updated++;

		/* t_tuples_hot_updated is nontransactional, so just advance it */
		if (hot)
			pgstat_info->t_counts.t_tuples_hot_updated++;
	}
}

/*
 * pgstat_count_heap_delete - count a tuple deletion
 */
void
pgstat_count_heap_delete(Relation rel)
{
	PgStat_TableStatus *pgstat_info = rel->pgstat_info;

	if (pgstat_info != NULL)
	{
		/* We have to log the effect at the proper transactional level */
		int			nest_level = GetCurrentTransactionNestLevel();

		if (pgstat_info->trans == NULL ||
			pgstat_info->trans->nest_level != nest_level)
			add_tabstat_xact_level(pgstat_info, nest_level);

		pgstat_info->trans->tuples_deleted++;
	}
}

/*
 * pgstat_truncate_save_counters
 *
 * Whenever a table is truncated, we save its i/u/d counters so that they can
 * be cleared, and if the (sub)xact that executed the truncate later aborts,
 * the counters can be restored to the saved (pre-truncate) values.  Note we do
 * this on the first truncate in any particular subxact level only.
 */
static void
pgstat_truncate_save_counters(PgStat_TableXactStatus *trans)
{
	if (!trans->truncated)
	{
		trans->inserted_pre_trunc = trans->tuples_inserted;
		trans->updated_pre_trunc = trans->tuples_updated;
		trans->deleted_pre_trunc = trans->tuples_deleted;
		trans->truncated = true;
	}
}

/*
 * pgstat_truncate_restore_counters - restore counters when a truncate aborts
 */
static void
pgstat_truncate_restore_counters(PgStat_TableXactStatus *trans)
{
	if (trans->truncated)
	{
		trans->tuples_inserted = trans->inserted_pre_trunc;
		trans->tuples_updated = trans->updated_pre_trunc;
		trans->tuples_deleted = trans->deleted_pre_trunc;
	}
}

/*
 * pgstat_count_truncate - update tuple counters due to truncate
 */
void
pgstat_count_truncate(Relation rel)
{
	PgStat_TableStatus *pgstat_info = rel->pgstat_info;

	if (pgstat_info != NULL)
	{
		/* We have to log the effect at the proper transactional level */
		int			nest_level = GetCurrentTransactionNestLevel();

		if (pgstat_info->trans == NULL ||
			pgstat_info->trans->nest_level != nest_level)
			add_tabstat_xact_level(pgstat_info, nest_level);

		pgstat_truncate_save_counters(pgstat_info->trans);
		pgstat_info->trans->tuples_inserted = 0;
		pgstat_info->trans->tuples_updated = 0;
		pgstat_info->trans->tuples_deleted = 0;
	}
}

/*
 * pgstat_update_heap_dead_tuples - update dead-tuples count
 *
 * The semantics of this are that we are reporting the nontransactional
 * recovery of "delta" dead tuples; so t_delta_dead_tuples decreases
 * rather than increasing, and the change goes straight into the per-table
 * counter, not into transactional state.
 */
void
pgstat_update_heap_dead_tuples(Relation rel, int delta)
{
	PgStat_TableStatus *pgstat_info = rel->pgstat_info;

	if (pgstat_info != NULL)
		pgstat_info->t_counts.t_delta_dead_tuples -= delta;
}


/* ----------
 * AtEOXact_PgStat
 *
 *	Called from access/transam/xact.c at top-level transaction commit/abort.
 * ----------
 */
void
AtEOXact_PgStat(bool isCommit)
{
	PgStat_SubXactStatus *xact_state;

	/*
	 * Count transaction commit or abort.  (We use counters, not just bools,
	 * in case the reporting message isn't sent right away.)
	 */
	if (isCommit)
		pgStatXactCommit++;
	else
		pgStatXactRollback++;

	/*
	 * Transfer transactional insert/update counts into the base tabstat
	 * entries.  We don't bother to free any of the transactional state, since
	 * it's all in TopTransactionContext and will go away anyway.
	 */
	xact_state = pgStatXactStack;
	if (xact_state != NULL)
	{
		PgStat_TableXactStatus *trans;

		Assert(xact_state->nest_level == 1);
		Assert(xact_state->prev == NULL);
		for (trans = xact_state->first; trans != NULL; trans = trans->next)
		{
			PgStat_TableStatus *tabstat;

			Assert(trans->nest_level == 1);
			Assert(trans->upper == NULL);
			tabstat = trans->parent;
			Assert(tabstat->trans == trans);
			/* restore pre-truncate stats (if any) in case of aborted xact */
			if (!isCommit)
				pgstat_truncate_restore_counters(trans);
			/* count attempted actions regardless of commit/abort */
			tabstat->t_counts.t_tuples_inserted += trans->tuples_inserted;
			tabstat->t_counts.t_tuples_updated += trans->tuples_updated;
			tabstat->t_counts.t_tuples_deleted += trans->tuples_deleted;
			if (isCommit)
			{
				tabstat->t_counts.t_truncated = trans->truncated;
				if (trans->truncated)
				{
					/* forget live/dead stats seen by backend thus far */
					tabstat->t_counts.t_delta_live_tuples = 0;
					tabstat->t_counts.t_delta_dead_tuples = 0;
				}
				/* insert adds a live tuple, delete removes one */
				tabstat->t_counts.t_delta_live_tuples +=
					trans->tuples_inserted - trans->tuples_deleted;
				/* update and delete each create a dead tuple */
				tabstat->t_counts.t_delta_dead_tuples +=
					trans->tuples_updated + trans->tuples_deleted;
				/* insert, update, delete each count as one change event */
				tabstat->t_counts.t_changed_tuples +=
					trans->tuples_inserted + trans->tuples_updated +
					trans->tuples_deleted;
			}
			else
			{
				/* inserted tuples are dead, deleted tuples are unaffected */
				tabstat->t_counts.t_delta_dead_tuples +=
					trans->tuples_inserted + trans->tuples_updated;
				/* an aborted xact generates no changed_tuple events */
			}
			tabstat->trans = NULL;
		}
	}
	pgStatXactStack = NULL;

	/* mark as the next reference is the first in a transaction */
	first_in_xact = true;
}

/* ----------
 * AtEOSubXact_PgStat
 *
 *	Called from access/transam/xact.c at subtransaction commit/abort.
 * ----------
 */
void
AtEOSubXact_PgStat(bool isCommit, int nestDepth)
{
	PgStat_SubXactStatus *xact_state;

	/*
	 * Transfer transactional insert/update counts into the next higher
	 * subtransaction state.
	 */
	xact_state = pgStatXactStack;
	if (xact_state != NULL &&
		xact_state->nest_level >= nestDepth)
	{
		PgStat_TableXactStatus *trans;
		PgStat_TableXactStatus *next_trans;

		/* delink xact_state from stack immediately to simplify reuse case */
		pgStatXactStack = xact_state->prev;

		for (trans = xact_state->first; trans != NULL; trans = next_trans)
		{
			PgStat_TableStatus *tabstat;

			next_trans = trans->next;
			Assert(trans->nest_level == nestDepth);
			tabstat = trans->parent;
			Assert(tabstat->trans == trans);
			if (isCommit)
			{
				if (trans->upper && trans->upper->nest_level == nestDepth - 1)
				{
					if (trans->truncated)
					{
						/* propagate the truncate status one level up */
						pgstat_truncate_save_counters(trans->upper);
						/* replace upper xact stats with ours */
						trans->upper->tuples_inserted = trans->tuples_inserted;
						trans->upper->tuples_updated = trans->tuples_updated;
						trans->upper->tuples_deleted = trans->tuples_deleted;
					}
					else
					{
						trans->upper->tuples_inserted += trans->tuples_inserted;
						trans->upper->tuples_updated += trans->tuples_updated;
						trans->upper->tuples_deleted += trans->tuples_deleted;
					}
					tabstat->trans = trans->upper;
					pfree(trans);
				}
				else
				{
					/*
					 * When there isn't an immediate parent state, we can just
					 * reuse the record instead of going through a
					 * palloc/pfree pushup (this works since it's all in
					 * TopTransactionContext anyway).  We have to re-link it
					 * into the parent level, though, and that might mean
					 * pushing a new entry into the pgStatXactStack.
					 */
					PgStat_SubXactStatus *upper_xact_state;

					upper_xact_state = get_tabstat_stack_level(nestDepth - 1);
					trans->next = upper_xact_state->first;
					upper_xact_state->first = trans;
					trans->nest_level = nestDepth - 1;
				}
			}
			else
			{
				/*
				 * On abort, update top-level tabstat counts, then forget the
				 * subtransaction
				 */

				/* first restore values obliterated by truncate */
				pgstat_truncate_restore_counters(trans);
				/* count attempted actions regardless of commit/abort */
				tabstat->t_counts.t_tuples_inserted += trans->tuples_inserted;
				tabstat->t_counts.t_tuples_updated += trans->tuples_updated;
				tabstat->t_counts.t_tuples_deleted += trans->tuples_deleted;
				/* inserted tuples are dead, deleted tuples are unaffected */
				tabstat->t_counts.t_delta_dead_tuples +=
					trans->tuples_inserted + trans->tuples_updated;
				tabstat->trans = trans->upper;
				pfree(trans);
			}
		}
		pfree(xact_state);
	}
}


/*
 * AtPrepare_PgStat
 *		Save the transactional stats state at 2PC transaction prepare.
 *
 * In this phase we just generate 2PC records for all the pending
 * transaction-dependent stats work.
 */
void
AtPrepare_PgStat(void)
{
	PgStat_SubXactStatus *xact_state;

	xact_state = pgStatXactStack;
	if (xact_state != NULL)
	{
		PgStat_TableXactStatus *trans;

		Assert(xact_state->nest_level == 1);
		Assert(xact_state->prev == NULL);
		for (trans = xact_state->first; trans != NULL; trans = trans->next)
		{
			PgStat_TableStatus *tabstat;
			TwoPhasePgStatRecord record;

			Assert(trans->nest_level == 1);
			Assert(trans->upper == NULL);
			tabstat = trans->parent;
			Assert(tabstat->trans == trans);

			record.tuples_inserted = trans->tuples_inserted;
			record.tuples_updated = trans->tuples_updated;
			record.tuples_deleted = trans->tuples_deleted;
			record.inserted_pre_trunc = trans->inserted_pre_trunc;
			record.updated_pre_trunc = trans->updated_pre_trunc;
			record.deleted_pre_trunc = trans->deleted_pre_trunc;
			record.t_id = tabstat->t_id;
			record.t_shared = tabstat->t_shared;
			record.t_truncated = trans->truncated;

			RegisterTwoPhaseRecord(TWOPHASE_RM_PGSTAT_ID, 0,
								   &record, sizeof(TwoPhasePgStatRecord));
		}
	}
}

/*
 * PostPrepare_PgStat
 *		Clean up after successful PREPARE.
 *
 * All we need do here is unlink the transaction stats state from the
 * nontransactional state.  The nontransactional action counts will be
 * reported to the stats collector immediately, while the effects on live
 * and dead tuple counts are preserved in the 2PC state file.
 *
 * Note: AtEOXact_PgStat is not called during PREPARE.
 */
void
PostPrepare_PgStat(void)
{
	PgStat_SubXactStatus *xact_state;

	/*
	 * We don't bother to free any of the transactional state, since it's all
	 * in TopTransactionContext and will go away anyway.
	 */
	xact_state = pgStatXactStack;
	if (xact_state != NULL)
	{
		PgStat_TableXactStatus *trans;

		for (trans = xact_state->first; trans != NULL; trans = trans->next)
		{
			PgStat_TableStatus *tabstat;

			tabstat = trans->parent;
			tabstat->trans = NULL;
		}
	}
	pgStatXactStack = NULL;

	/* mark as the next reference is the first in a transaction */
	first_in_xact = true;
}

/*
 * 2PC processing routine for COMMIT PREPARED case.
 *
 * Load the saved counts into our local pgstats state.
 */
void
pgstat_twophase_postcommit(TransactionId xid, uint16 info,
						   void *recdata, uint32 len)
{
	TwoPhasePgStatRecord *rec = (TwoPhasePgStatRecord *) recdata;
	PgStat_TableStatus *pgstat_info;

	/* Find or create a tabstat entry for the rel */
	pgstat_info = get_tabstat_entry(rec->t_id, rec->t_shared);

	/* Same math as in AtEOXact_PgStat, commit case */
	pgstat_info->t_counts.t_tuples_inserted += rec->tuples_inserted;
	pgstat_info->t_counts.t_tuples_updated += rec->tuples_updated;
	pgstat_info->t_counts.t_tuples_deleted += rec->tuples_deleted;
	pgstat_info->t_counts.t_truncated = rec->t_truncated;
	if (rec->t_truncated)
	{
		/* forget live/dead stats seen by backend thus far */
		pgstat_info->t_counts.t_delta_live_tuples = 0;
		pgstat_info->t_counts.t_delta_dead_tuples = 0;
	}
	pgstat_info->t_counts.t_delta_live_tuples +=
		rec->tuples_inserted - rec->tuples_deleted;
	pgstat_info->t_counts.t_delta_dead_tuples +=
		rec->tuples_updated + rec->tuples_deleted;
	pgstat_info->t_counts.t_changed_tuples +=
		rec->tuples_inserted + rec->tuples_updated +
		rec->tuples_deleted;
}

/*
 * 2PC processing routine for ROLLBACK PREPARED case.
 *
 * Load the saved counts into our local pgstats state, but treat them
 * as aborted.
 */
void
pgstat_twophase_postabort(TransactionId xid, uint16 info,
						  void *recdata, uint32 len)
{
	TwoPhasePgStatRecord *rec = (TwoPhasePgStatRecord *) recdata;
	PgStat_TableStatus *pgstat_info;

	/* Find or create a tabstat entry for the rel */
	pgstat_info = get_tabstat_entry(rec->t_id, rec->t_shared);

	/* Same math as in AtEOXact_PgStat, abort case */
	if (rec->t_truncated)
	{
		rec->tuples_inserted = rec->inserted_pre_trunc;
		rec->tuples_updated = rec->updated_pre_trunc;
		rec->tuples_deleted = rec->deleted_pre_trunc;
	}
	pgstat_info->t_counts.t_tuples_inserted += rec->tuples_inserted;
	pgstat_info->t_counts.t_tuples_updated += rec->tuples_updated;
	pgstat_info->t_counts.t_tuples_deleted += rec->tuples_deleted;
	pgstat_info->t_counts.t_delta_dead_tuples +=
		rec->tuples_inserted + rec->tuples_updated;
}


/* ----------
 * pgstat_fetch_stat_dbentry() -
 *
 *	Find database stats entry on backends. The returned entries are cached
 *	until transaction end or pgstat_clear_snapshot() is called.
 */
PgStat_StatDBEntry *
pgstat_fetch_stat_dbentry(Oid dbid)
{
	/* context for snapshot_statentry */
	static pgstat_snapshot_cxt cxt =
	{
		.hash_name = "local database stats hash",
		.hash = NULL,
		.hash_entsize = sizeof(PgStat_StatDBEntry_snapshot),
		.dshash = NULL,
		.dsh_handle = DSM_HANDLE_INVALID,
		.dsh_params = &dsh_dbparams
	};

	/* should be called from backends  */
	Assert(IsUnderPostmaster);

	/* If not done for this transaction, take a snapshot of global stats */
	pgstat_snapshot_global_stats();

	cxt.dshash = &pgStatDBHash;
	cxt.hash = &pgStatDBEntrySnapshot;

	/* caller doesn't have a business with snapshot-local members  */
	return (PgStat_StatDBEntry *)
		snapshot_statentry(&cxt, dbid);
}

/* ----------
 * pgstat_fetch_stat_tabentry() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	the collected statistics for one table or NULL. NULL doesn't mean
 *	that the table doesn't exist, it is just not yet known by the
 *	collector, so the caller is better off to report ZERO instead.
 * ----------
 */
PgStat_StatTabEntry *
pgstat_fetch_stat_tabentry(Oid relid)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;

	/* Lookup our database, then look in its table hash table. */
	dbentry = pgstat_fetch_stat_dbentry(MyDatabaseId);
	if (dbentry == NULL)
		return NULL;

	tabentry = pgstat_fetch_stat_tabentry_extended(dbentry, relid);
	if (tabentry != NULL)
		return tabentry;

	/*
	 * If we didn't find it, maybe it's a shared table.
	 */
	dbentry = pgstat_fetch_stat_dbentry(InvalidOid);
	if (dbentry == NULL)
		return NULL;

	tabentry = pgstat_fetch_stat_tabentry_extended(dbentry, relid);
	if (tabentry != NULL)
		return tabentry;

	return NULL;
}

/* ----------
 * pgstat_fetch_stat_tabentry_extended() -
 *
 *	Find table stats entry on backends. The returned entries are cached until
 *	transaction end or pgstat_clear_snapshot() is called.
 */
PgStat_StatTabEntry *
pgstat_fetch_stat_tabentry_extended(PgStat_StatDBEntry *dbent, Oid reloid)
{
	/* context for snapshot_statentry */
	static pgstat_snapshot_cxt cxt =
	{
		.hash_name = "table stats snapshot hash",
		.hash = NULL,
		.hash_entsize = sizeof(PgStat_StatDBEntry_snapshot),
		.dshash = NULL,
		.dsh_handle = DSM_HANDLE_INVALID,
		.dsh_params = &dsh_dbparams
	};
	PgStat_StatDBEntry_snapshot *local_dbent;

	/* should be called from backends  */
	Assert(IsUnderPostmaster);

	/* dbent given to this function is alias of PgStat_StatDBEntry_snapshot */
	local_dbent = (PgStat_StatDBEntry_snapshot *)dbent;
	cxt.hash = &local_dbent->snapshot_tables;
	cxt.dshash = &local_dbent->dshash_tables;
	cxt.dsh_handle = dbent->tables;

	return (PgStat_StatTabEntry *)
		snapshot_statentry(&cxt, reloid);
}


/* ----------
 * pgstat_fetch_stat_funcentry() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	the collected statistics for one function or NULL.
 * ----------
 */
PgStat_StatFuncEntry *
pgstat_fetch_stat_funcentry(Oid func_id)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatFuncEntry *funcentry = NULL;

	/* Lookup our database, then find the requested function */
	dbentry = pgstat_fetch_stat_dbentry(MyDatabaseId);
	if (dbentry == NULL)
		return NULL;

	funcentry = pgstat_fetch_stat_funcentry_extended(dbentry, func_id);

	return funcentry;
}

/* ----------
 * pgstat_fetch_stat_funcentry_extended() -
 *
 *	Find function stats entry on backends. The returned entries are cached
 *	until transaction end or pgstat_clear_snapshot() is called.
 */
static PgStat_StatFuncEntry *
pgstat_fetch_stat_funcentry_extended(PgStat_StatDBEntry *dbent, Oid funcid)
{
	/* context for snapshot_statentry */
	static pgstat_snapshot_cxt cxt =
	{
		.hash_name = "function stats snapshot hash",
		.hash = NULL,
		.hash_entsize = sizeof(PgStat_StatDBEntry_snapshot),
		.dshash = NULL,
		.dsh_handle = DSM_HANDLE_INVALID,
		.dsh_params = &dsh_dbparams
	};
	PgStat_StatDBEntry_snapshot *local_dbent;

	/* should be called from backends  */
	Assert(IsUnderPostmaster);

	if (dbent->functions == DSM_HANDLE_INVALID)
		return NULL;

	/* dbent given to this function is alias of PgStat_StatDBEntry_snapshot */
	local_dbent = (PgStat_StatDBEntry_snapshot *)dbent;
	cxt.hash = &local_dbent->snapshot_functions;
	cxt.dshash = &local_dbent->dshash_functions;
	cxt.dsh_handle = dbent->functions;

	return (PgStat_StatFuncEntry *)
		snapshot_statentry(&cxt, funcid);
}

/*
 * pgstat_snapshot_global_stats() -
 *
 * Makes a snapshot of global stats if not done yet.  They will be kept until
 * subsequent call of pgstat_clear_snapshot() or the end of the current
 * memory context (typically TopTransactionContext).
 */
static void
pgstat_snapshot_global_stats(void)
{
	MemoryContext oldcontext;
	TimestampTz update_time = 0;

	/* The snapshot lives within CacheMemoryContext */
	if (pgStatSnapshotContext == NULL)
	{
		pgStatSnapshotContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "Stats snapshot context",
								  ALLOCSET_DEFAULT_SIZES);
	}

	/*
	 * Shared stats are updated frequently especially when many backends are
	 * running, but we don't want to reconstruct snapshot so frequently for
	 * performance reasons. Keep them at least for the same duration with
	 * minimal stats update interval of a backend. As the result snapshots may
	 * live for multiple transactions.
	 */
	if (first_in_xact && IsTransactionState())
	{
		first_in_xact = false;
		LWLockAcquire(StatsLock, LW_SHARED);
		update_time = StatsShmem->last_update;
		LWLockRelease(StatsLock);

		if (snapshot_expires_at < update_time)
		{
			/* No problem to expire involving backend status */
			pgstat_clear_snapshot();

			snapshot_expires_at =
				update_time + PGSTAT_STAT_MIN_INTERVAL * USECS_PER_SEC / 1000;
		}
	}

	/* Nothing to do if already done */
	if (snapshot_globalStats)
		return;

	Assert(snapshot_archiverStats == NULL);

	oldcontext = MemoryContextSwitchTo(pgStatSnapshotContext);

	/* global stats can be just copied  */
	LWLockAcquire(StatsLock, LW_SHARED);
	snapshot_globalStats = palloc(sizeof(PgStat_GlobalStats));
	memcpy(snapshot_globalStats, shared_globalStats,
		   sizeof(PgStat_GlobalStats));

	snapshot_archiverStats = palloc(sizeof(PgStat_ArchiverStats));
	memcpy(snapshot_archiverStats, shared_archiverStats,
		   sizeof(PgStat_ArchiverStats));
	LWLockRelease(StatsLock);

	/* set the timestamp of this snapshot */
	snapshot_globalStats->stats_timestamp = update_time;

	MemoryContextSwitchTo(oldcontext);

	return;
}

/* ----------
 * pgstat_fetch_stat_beentry() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	our local copy of the current-activity entry for one backend.
 *
 *	NB: caller is responsible for a check if the user is permitted to see
 *	this info (especially the querystring).
 * ----------
 */
PgBackendStatus *
pgstat_fetch_stat_beentry(int beid)
{
	pgstat_read_current_status();

	if (beid < 1 || beid > localNumBackends)
		return NULL;

	return &localBackendStatusTable[beid - 1].backendStatus;
}


/* ----------
 * pgstat_fetch_stat_local_beentry() -
 *
 *	Like pgstat_fetch_stat_beentry() but with locally computed additions (like
 *	xid and xmin values of the backend)
 *
 *	NB: caller is responsible for a check if the user is permitted to see
 *	this info (especially the querystring).
 * ----------
 */
LocalPgBackendStatus *
pgstat_fetch_stat_local_beentry(int beid)
{
	pgstat_read_current_status();

	if (beid < 1 || beid > localNumBackends)
		return NULL;

	return &localBackendStatusTable[beid - 1];
}


/* ----------
 * pgstat_fetch_stat_numbackends() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	the maximum current backend id.
 * ----------
 */
int
pgstat_fetch_stat_numbackends(void)
{
	pgstat_read_current_status();

	return localNumBackends;
}

/*
 * ---------
 * pgstat_fetch_stat_archiver() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	a pointer to the archiver statistics struct.
 * ---------
 */
PgStat_ArchiverStats *
pgstat_fetch_stat_archiver(void)
{
	/* If not done for this transaction, take a stats snapshot */
	pgstat_snapshot_global_stats();

	return snapshot_archiverStats;
}


/*
 * ---------
 * pgstat_fetch_global() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	a pointer to the global statistics struct.
 * ---------
 */
PgStat_GlobalStats *
pgstat_fetch_global(void)
{
	/* If not done for this transaction, take a stats snapshot */
	pgstat_snapshot_global_stats();

	return snapshot_globalStats;
}


/* ------------------------------------------------------------
 * Functions for management of the shared-memory PgBackendStatus array
 * ------------------------------------------------------------
 */

static PgBackendStatus *BackendStatusArray = NULL;
static PgBackendStatus *MyBEEntry = NULL;
static char *BackendAppnameBuffer = NULL;
static char *BackendClientHostnameBuffer = NULL;
static char *BackendActivityBuffer = NULL;
static Size BackendActivityBufferSize = 0;
#ifdef USE_SSL
static PgBackendSSLStatus *BackendSslStatusBuffer = NULL;
#endif


/*
 * Report shared-memory space needed by CreateSharedBackendStatus.
 */
Size
BackendStatusShmemSize(void)
{
	Size		size;

	/* BackendStatusArray: */
	size = mul_size(sizeof(PgBackendStatus), NumBackendStatSlots);
	/* BackendAppnameBuffer: */
	size = add_size(size,
					mul_size(NAMEDATALEN, NumBackendStatSlots));
	/* BackendClientHostnameBuffer: */
	size = add_size(size,
					mul_size(NAMEDATALEN, NumBackendStatSlots));
	/* BackendActivityBuffer: */
	size = add_size(size,
					mul_size(pgstat_track_activity_query_size, NumBackendStatSlots));
#ifdef USE_SSL
	/* BackendSslStatusBuffer: */
	size = add_size(size,
					mul_size(sizeof(PgBackendSSLStatus), NumBackendStatSlots));
#endif
	return size;
}

/*
 * Initialize the shared status array and several string buffers
 * during postmaster startup.
 */
void
CreateSharedBackendStatus(void)
{
	Size		size;
	bool		found;
	int			i;
	char	   *buffer;

	/* Create or attach to the shared array */
	size = mul_size(sizeof(PgBackendStatus), NumBackendStatSlots);
	BackendStatusArray = (PgBackendStatus *)
		ShmemInitStruct("Backend Status Array", size, &found);

	if (!found)
	{
		/*
		 * We're the first - initialize.
		 */
		MemSet(BackendStatusArray, 0, size);
	}

	/* Create or attach to the shared appname buffer */
	size = mul_size(NAMEDATALEN, NumBackendStatSlots);
	BackendAppnameBuffer = (char *)
		ShmemInitStruct("Backend Application Name Buffer", size, &found);

	if (!found)
	{
		MemSet(BackendAppnameBuffer, 0, size);

		/* Initialize st_appname pointers. */
		buffer = BackendAppnameBuffer;
		for (i = 0; i < NumBackendStatSlots; i++)
		{
			BackendStatusArray[i].st_appname = buffer;
			buffer += NAMEDATALEN;
		}
	}

	/* Create or attach to the shared client hostname buffer */
	size = mul_size(NAMEDATALEN, NumBackendStatSlots);
	BackendClientHostnameBuffer = (char *)
		ShmemInitStruct("Backend Client Host Name Buffer", size, &found);

	if (!found)
	{
		MemSet(BackendClientHostnameBuffer, 0, size);

		/* Initialize st_clienthostname pointers. */
		buffer = BackendClientHostnameBuffer;
		for (i = 0; i < NumBackendStatSlots; i++)
		{
			BackendStatusArray[i].st_clienthostname = buffer;
			buffer += NAMEDATALEN;
		}
	}

	/* Create or attach to the shared activity buffer */
	BackendActivityBufferSize = mul_size(pgstat_track_activity_query_size,
										 NumBackendStatSlots);
	BackendActivityBuffer = (char *)
		ShmemInitStruct("Backend Activity Buffer",
						BackendActivityBufferSize,
						&found);

	if (!found)
	{
		MemSet(BackendActivityBuffer, 0, BackendActivityBufferSize);

		/* Initialize st_activity pointers. */
		buffer = BackendActivityBuffer;
		for (i = 0; i < NumBackendStatSlots; i++)
		{
			BackendStatusArray[i].st_activity_raw = buffer;
			buffer += pgstat_track_activity_query_size;
		}
	}

#ifdef USE_SSL
	/* Create or attach to the shared SSL status buffer */
	size = mul_size(sizeof(PgBackendSSLStatus), NumBackendStatSlots);
	BackendSslStatusBuffer = (PgBackendSSLStatus *)
		ShmemInitStruct("Backend SSL Status Buffer", size, &found);

	if (!found)
	{
		PgBackendSSLStatus *ptr;

		MemSet(BackendSslStatusBuffer, 0, size);

		/* Initialize st_sslstatus pointers. */
		ptr = BackendSslStatusBuffer;
		for (i = 0; i < NumBackendStatSlots; i++)
		{
			BackendStatusArray[i].st_sslstatus = ptr;
			ptr++;
		}
	}
#endif
}


/* ----------
 * pgstat_initialize() -
 *
 *	Initialize pgstats state, and set up our on-proc-exit hook.
 *	Called from InitPostgres and AuxiliaryProcessMain. For auxiliary process,
 *	MyBackendId is invalid. Otherwise, MyBackendId must be set,
 *	but we must not have started any transaction yet (since the
 *	exit hook must run after the last transaction exit).
 *	NOTE: MyDatabaseId isn't set yet; so the shutdown hook has to be careful.
 * ----------
 */
void
pgstat_initialize(void)
{
	/* Initialize MyBEEntry */
	if (MyBackendId != InvalidBackendId)
	{
		Assert(MyBackendId >= 1 && MyBackendId <= MaxBackends);
		MyBEEntry = &BackendStatusArray[MyBackendId - 1];
	}
	else
	{
		/* Must be an auxiliary process */
		Assert(MyAuxProcType != NotAnAuxProcess);

		/*
		 * Assign the MyBEEntry for an auxiliary process.  Since it doesn't
		 * have a BackendId, the slot is statically allocated based on the
		 * auxiliary process type (MyAuxProcType).  Backends use slots indexed
		 * in the range from 1 to MaxBackends (inclusive), so we use
		 * MaxBackends + AuxBackendType + 1 as the index of the slot for an
		 * auxiliary process.
		 */
		MyBEEntry = &BackendStatusArray[MaxBackends + MyAuxProcType];
	}

	/* need to be called before dsm shutodwn */
	before_shmem_exit(pgstat_beshutdown_hook, 0);
}

/* ----------
 * pgstat_bestart() -
 *
 *	Initialize this backend's entry in the PgBackendStatus array.
 *	Called from InitPostgres.
 *
 *	Apart from auxiliary processes, MyBackendId, MyDatabaseId,
 *	session userid, and application_name must be set for a
 *	backend (hence, this cannot be combined with pgstat_initialize).
 * ----------
 */
void
pgstat_bestart(void)
{
	SockAddr	clientaddr;
	volatile PgBackendStatus *beentry;

	/*
	 * To minimize the time spent modifying the PgBackendStatus entry, fetch
	 * all the needed data first.
	 */

	/*
	 * We may not have a MyProcPort (eg, if this is the autovacuum process).
	 * If so, use all-zeroes client address, which is dealt with specially in
	 * pg_stat_get_backend_client_addr and pg_stat_get_backend_client_port.
	 */
	if (MyProcPort)
		memcpy(&clientaddr, &MyProcPort->raddr, sizeof(clientaddr));
	else
		MemSet(&clientaddr, 0, sizeof(clientaddr));

	/*
	 * Initialize my status entry, following the protocol of bumping
	 * st_changecount before and after; and make sure it's even afterwards. We
	 * use a volatile pointer here to ensure the compiler doesn't try to get
	 * cute.
	 */
	beentry = MyBEEntry;

	/* pgstats state must be initialized from pgstat_initialize() */
	Assert(beentry != NULL);

	if (MyBackendId != InvalidBackendId)
	{
		if (IsAutoVacuumLauncherProcess())
		{
			/* Autovacuum Launcher */
			beentry->st_backendType = B_AUTOVAC_LAUNCHER;
		}
		else if (IsAutoVacuumWorkerProcess())
		{
			/* Autovacuum Worker */
			beentry->st_backendType = B_AUTOVAC_WORKER;
		}
		else if (am_walsender)
		{
			/* Wal sender */
			beentry->st_backendType = B_WAL_SENDER;
		}
		else if (IsBackgroundWorker)
		{
			/* bgworker */
			beentry->st_backendType = B_BG_WORKER;
		}
		else
		{
			/* client-backend */
			beentry->st_backendType = B_BACKEND;
		}
	}
	else
	{
		/* Must be an auxiliary process */
		Assert(MyAuxProcType != NotAnAuxProcess);
		switch (MyAuxProcType)
		{
			case StartupProcess:
				beentry->st_backendType = B_STARTUP;
				break;
			case ArchiverProcess:
				beentry->st_backendType = B_ARCHIVER;
				break;
			case BgWriterProcess:
				beentry->st_backendType = B_BG_WRITER;
				break;
			case CheckpointerProcess:
				beentry->st_backendType = B_CHECKPOINTER;
				break;
			case WalWriterProcess:
				beentry->st_backendType = B_WAL_WRITER;
				break;
			case WalReceiverProcess:
				beentry->st_backendType = B_WAL_RECEIVER;
				break;
			default:
				elog(FATAL, "unrecognized process type: %d",
					 (int) MyAuxProcType);
				proc_exit(1);
		}
	}

	do
	{
		pgstat_increment_changecount_before(beentry);
	} while ((beentry->st_changecount & 1) == 0);

	beentry->st_procpid = MyProcPid;
	beentry->st_proc_start_timestamp = MyStartTimestamp;
	beentry->st_activity_start_timestamp = 0;
	beentry->st_state_start_timestamp = 0;
	beentry->st_xact_start_timestamp = 0;
	beentry->st_databaseid = MyDatabaseId;

	/* We have userid for client-backends, wal-sender and bgworker processes */
	if (beentry->st_backendType == B_BACKEND
		|| beentry->st_backendType == B_WAL_SENDER
		|| beentry->st_backendType == B_BG_WORKER)
		beentry->st_userid = GetSessionUserId();
	else
		beentry->st_userid = InvalidOid;

	beentry->st_clientaddr = clientaddr;
	if (MyProcPort && MyProcPort->remote_hostname)
		strlcpy(beentry->st_clienthostname, MyProcPort->remote_hostname,
				NAMEDATALEN);
	else
		beentry->st_clienthostname[0] = '\0';
#ifdef USE_SSL
	if (MyProcPort && MyProcPort->ssl != NULL)
	{
		beentry->st_ssl = true;
		beentry->st_sslstatus->ssl_bits = be_tls_get_cipher_bits(MyProcPort);
		beentry->st_sslstatus->ssl_compression = be_tls_get_compression(MyProcPort);
		strlcpy(beentry->st_sslstatus->ssl_version, be_tls_get_version(MyProcPort), NAMEDATALEN);
		strlcpy(beentry->st_sslstatus->ssl_cipher, be_tls_get_cipher(MyProcPort), NAMEDATALEN);
		be_tls_get_peer_subject_name(MyProcPort, beentry->st_sslstatus->ssl_client_dn, NAMEDATALEN);
		be_tls_get_peer_serial(MyProcPort, beentry->st_sslstatus->ssl_client_serial, NAMEDATALEN);
		be_tls_get_peer_issuer_name(MyProcPort, beentry->st_sslstatus->ssl_issuer_dn, NAMEDATALEN);
	}
	else
	{
		beentry->st_ssl = false;
	}
#else
	beentry->st_ssl = false;
#endif
	beentry->st_state = STATE_UNDEFINED;
	beentry->st_appname[0] = '\0';
	beentry->st_activity_raw[0] = '\0';
	/* Also make sure the last byte in each string area is always 0 */
	beentry->st_clienthostname[NAMEDATALEN - 1] = '\0';
	beentry->st_appname[NAMEDATALEN - 1] = '\0';
	beentry->st_activity_raw[pgstat_track_activity_query_size - 1] = '\0';
	beentry->st_progress_command = PROGRESS_COMMAND_INVALID;
	beentry->st_progress_command_target = InvalidOid;

	/*
	 * we don't zero st_progress_param here to save cycles; nobody should
	 * examine it until st_progress_command has been set to something other
	 * than PROGRESS_COMMAND_INVALID
	 */

	pgstat_increment_changecount_after(beentry);

	/* Update app name to current GUC setting */
	if (application_name)
		pgstat_report_appname(application_name);
}

/*
 * Shut down a single backend's statistics reporting at process exit.
 *
 * Flush any remaining statistics counts out to the collector.
 * Without this, operations triggered during backend exit (such as
 * temp table deletions) won't be counted.
 *
 * Lastly, clear out our entry in the PgBackendStatus array.
 */
static void
pgstat_beshutdown_hook(int code, Datum arg)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	/*
	 * If we got as far as discovering our own database ID, we can report what
	 * we did to the collector.  Otherwise, we'd be sending an invalid
	 * database ID, so forget it.  (This means that accesses to pg_database
	 * during failed backend starts might never get counted.)
	 */
	if (OidIsValid(MyDatabaseId))
		pgstat_report_stat(true);

	/*
	 * Clear my status entry, following the protocol of bumping st_changecount
	 * before and after.  We use a volatile pointer here to ensure the
	 * compiler doesn't try to get cute.
	 */
	pgstat_increment_changecount_before(beentry);

	beentry->st_procpid = 0;	/* mark invalid */

	pgstat_increment_changecount_after(beentry);
}


/* ----------
 * pgstat_report_activity() -
 *
 *	Called from tcop/postgres.c to report what the backend is actually doing
 *	(but note cmd_str can be NULL for certain cases).
 *
 * All updates of the status entry follow the protocol of bumping
 * st_changecount before and after.  We use a volatile pointer here to
 * ensure the compiler doesn't try to get cute.
 * ----------
 */
void
pgstat_report_activity(BackendState state, const char *cmd_str)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	TimestampTz start_timestamp;
	TimestampTz current_timestamp;
	int			len = 0;

	TRACE_POSTGRESQL_STATEMENT_STATUS(cmd_str);

	if (!beentry)
		return;

	if (!pgstat_track_activities)
	{
		if (beentry->st_state != STATE_DISABLED)
		{
			volatile PGPROC *proc = MyProc;

			/*
			 * track_activities is disabled, but we last reported a
			 * non-disabled state.  As our final update, change the state and
			 * clear fields we will not be updating anymore.
			 */
			pgstat_increment_changecount_before(beentry);
			beentry->st_state = STATE_DISABLED;
			beentry->st_state_start_timestamp = 0;
			beentry->st_activity_raw[0] = '\0';
			beentry->st_activity_start_timestamp = 0;
			/* st_xact_start_timestamp and wait_event_info are also disabled */
			beentry->st_xact_start_timestamp = 0;
			proc->wait_event_info = 0;
			pgstat_increment_changecount_after(beentry);
		}
		return;
	}

	/*
	 * To minimize the time spent modifying the entry, fetch all the needed
	 * data first.
	 */
	start_timestamp = GetCurrentStatementStartTimestamp();
	if (cmd_str != NULL)
	{
		/*
		 * Compute length of to-be-stored string unaware of multi-byte
		 * characters. For speed reasons that'll get corrected on read, rather
		 * than computed every write.
		 */
		len = Min(strlen(cmd_str), pgstat_track_activity_query_size - 1);
	}
	current_timestamp = GetCurrentTimestamp();

	/*
	 * Now update the status entry
	 */
	pgstat_increment_changecount_before(beentry);

	beentry->st_state = state;
	beentry->st_state_start_timestamp = current_timestamp;

	if (cmd_str != NULL)
	{
		memcpy((char *) beentry->st_activity_raw, cmd_str, len);
		beentry->st_activity_raw[len] = '\0';
		beentry->st_activity_start_timestamp = start_timestamp;
	}

	pgstat_increment_changecount_after(beentry);
}

/*-----------
 * pgstat_progress_start_command() -
 *
 * Set st_progress_command (and st_progress_command_target) in own backend
 * entry.  Also, zero-initialize st_progress_param array.
 *-----------
 */
void
pgstat_progress_start_command(ProgressCommandType cmdtype, Oid relid)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	if (!beentry || !pgstat_track_activities)
		return;

	pgstat_increment_changecount_before(beentry);
	beentry->st_progress_command = cmdtype;
	beentry->st_progress_command_target = relid;
	MemSet(&beentry->st_progress_param, 0, sizeof(beentry->st_progress_param));
	pgstat_increment_changecount_after(beentry);
}

/*-----------
 * pgstat_progress_update_param() -
 *
 * Update index'th member in st_progress_param[] of own backend entry.
 *-----------
 */
void
pgstat_progress_update_param(int index, int64 val)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	Assert(index >= 0 && index < PGSTAT_NUM_PROGRESS_PARAM);

	if (!beentry || !pgstat_track_activities)
		return;

	pgstat_increment_changecount_before(beentry);
	beentry->st_progress_param[index] = val;
	pgstat_increment_changecount_after(beentry);
}

/*-----------
 * pgstat_progress_update_multi_param() -
 *
 * Update multiple members in st_progress_param[] of own backend entry.
 * This is atomic; readers won't see intermediate states.
 *-----------
 */
void
pgstat_progress_update_multi_param(int nparam, const int *index,
								   const int64 *val)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	int			i;

	if (!beentry || !pgstat_track_activities || nparam == 0)
		return;

	pgstat_increment_changecount_before(beentry);

	for (i = 0; i < nparam; ++i)
	{
		Assert(index[i] >= 0 && index[i] < PGSTAT_NUM_PROGRESS_PARAM);

		beentry->st_progress_param[index[i]] = val[i];
	}

	pgstat_increment_changecount_after(beentry);
}

/*-----------
 * pgstat_progress_end_command() -
 *
 * Reset st_progress_command (and st_progress_command_target) in own backend
 * entry.  This signals the end of the command.
 *-----------
 */
void
pgstat_progress_end_command(void)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	if (!beentry)
		return;
	if (!pgstat_track_activities
		&& beentry->st_progress_command == PROGRESS_COMMAND_INVALID)
		return;

	pgstat_increment_changecount_before(beentry);
	beentry->st_progress_command = PROGRESS_COMMAND_INVALID;
	beentry->st_progress_command_target = InvalidOid;
	pgstat_increment_changecount_after(beentry);
}

/* ----------
 * pgstat_report_appname() -
 *
 *	Called to update our application name.
 * ----------
 */
void
pgstat_report_appname(const char *appname)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	int			len;

	if (!beentry)
		return;

	/* This should be unnecessary if GUC did its job, but be safe */
	len = pg_mbcliplen(appname, strlen(appname), NAMEDATALEN - 1);

	/*
	 * Update my status entry, following the protocol of bumping
	 * st_changecount before and after.  We use a volatile pointer here to
	 * ensure the compiler doesn't try to get cute.
	 */
	pgstat_increment_changecount_before(beentry);

	memcpy((char *) beentry->st_appname, appname, len);
	beentry->st_appname[len] = '\0';

	pgstat_increment_changecount_after(beentry);
}

/*
 * Report current transaction start timestamp as the specified value.
 * Zero means there is no active transaction.
 */
void
pgstat_report_xact_timestamp(TimestampTz tstamp)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	if (!pgstat_track_activities || !beentry)
		return;

	/*
	 * Update my status entry, following the protocol of bumping
	 * st_changecount before and after.  We use a volatile pointer here to
	 * ensure the compiler doesn't try to get cute.
	 */
	pgstat_increment_changecount_before(beentry);
	beentry->st_xact_start_timestamp = tstamp;
	pgstat_increment_changecount_after(beentry);
}

/* ----------
 * pgstat_read_current_status() -
 *
 *	Copy the current contents of the PgBackendStatus array to local memory,
 *	if not already done in this transaction.
 * ----------
 */
static void
pgstat_read_current_status(void)
{
	volatile PgBackendStatus *beentry;
	LocalPgBackendStatus *localtable;
	LocalPgBackendStatus *localentry;
	char	   *localappname,
			   *localclienthostname,
			   *localactivity;
#ifdef USE_SSL
	PgBackendSSLStatus *localsslstatus;
#endif
	int			i;

	Assert(IsUnderPostmaster);

	if (localBackendStatusTable)
		return;					/* already done */

	pgstat_setup_memcxt();

	localtable = (LocalPgBackendStatus *)
		MemoryContextAlloc(pgStatLocalContext,
						   sizeof(LocalPgBackendStatus) * NumBackendStatSlots);
	localappname = (char *)
		MemoryContextAlloc(pgStatLocalContext,
						   NAMEDATALEN * NumBackendStatSlots);
	localclienthostname = (char *)
		MemoryContextAlloc(pgStatLocalContext,
						   NAMEDATALEN * NumBackendStatSlots);
	localactivity = (char *)
		MemoryContextAlloc(pgStatLocalContext,
						   pgstat_track_activity_query_size * NumBackendStatSlots);
#ifdef USE_SSL
	localsslstatus = (PgBackendSSLStatus *)
		MemoryContextAlloc(pgStatLocalContext,
						   sizeof(PgBackendSSLStatus) * NumBackendStatSlots);
#endif

	localNumBackends = 0;

	beentry = BackendStatusArray;
	localentry = localtable;
	for (i = 1; i <= NumBackendStatSlots; i++)
	{
		/*
		 * Follow the protocol of retrying if st_changecount changes while we
		 * copy the entry, or if it's odd.  (The check for odd is needed to
		 * cover the case where we are able to completely copy the entry while
		 * the source backend is between increment steps.)	We use a volatile
		 * pointer here to ensure the compiler doesn't try to get cute.
		 */
		for (;;)
		{
			int			before_changecount;
			int			after_changecount;

			pgstat_save_changecount_before(beentry, before_changecount);

			localentry->backendStatus.st_procpid = beentry->st_procpid;
			if (localentry->backendStatus.st_procpid > 0)
			{
				memcpy(&localentry->backendStatus, (char *) beentry, sizeof(PgBackendStatus));

				/*
				 * strcpy is safe even if the string is modified concurrently,
				 * because there's always a \0 at the end of the buffer.
				 */
				strcpy(localappname, (char *) beentry->st_appname);
				localentry->backendStatus.st_appname = localappname;
				strcpy(localclienthostname, (char *) beentry->st_clienthostname);
				localentry->backendStatus.st_clienthostname = localclienthostname;
				strcpy(localactivity, (char *) beentry->st_activity_raw);
				localentry->backendStatus.st_activity_raw = localactivity;
				localentry->backendStatus.st_ssl = beentry->st_ssl;
#ifdef USE_SSL
				if (beentry->st_ssl)
				{
					memcpy(localsslstatus, beentry->st_sslstatus, sizeof(PgBackendSSLStatus));
					localentry->backendStatus.st_sslstatus = localsslstatus;
				}
#endif
			}

			pgstat_save_changecount_after(beentry, after_changecount);
			if (before_changecount == after_changecount &&
				(before_changecount & 1) == 0)
				break;

			/* Make sure we can break out of loop if stuck... */
			CHECK_FOR_INTERRUPTS();
		}

		beentry++;
		/* Only valid entries get included into the local array */
		if (localentry->backendStatus.st_procpid > 0)
		{
			BackendIdGetTransactionIds(i,
									   &localentry->backend_xid,
									   &localentry->backend_xmin);

			localentry++;
			localappname += NAMEDATALEN;
			localclienthostname += NAMEDATALEN;
			localactivity += pgstat_track_activity_query_size;
#ifdef USE_SSL
			localsslstatus++;
#endif
			localNumBackends++;
		}
	}

	/* Set the pointer only after completion of a valid table */
	localBackendStatusTable = localtable;
}

/* ----------
 * pgstat_get_wait_event_type() -
 *
 *	Return a string representing the current wait event type, backend is
 *	waiting on.
 */
const char *
pgstat_get_wait_event_type(uint32 wait_event_info)
{
	uint32		classId;
	const char *event_type;

	/* report process as not waiting. */
	if (wait_event_info == 0)
		return NULL;

	classId = wait_event_info & 0xFF000000;

	switch (classId)
	{
		case PG_WAIT_LWLOCK:
			event_type = "LWLock";
			break;
		case PG_WAIT_LOCK:
			event_type = "Lock";
			break;
		case PG_WAIT_BUFFER_PIN:
			event_type = "BufferPin";
			break;
		case PG_WAIT_ACTIVITY:
			event_type = "Activity";
			break;
		case PG_WAIT_CLIENT:
			event_type = "Client";
			break;
		case PG_WAIT_EXTENSION:
			event_type = "Extension";
			break;
		case PG_WAIT_IPC:
			event_type = "IPC";
			break;
		case PG_WAIT_TIMEOUT:
			event_type = "Timeout";
			break;
		case PG_WAIT_IO:
			event_type = "IO";
			break;
		default:
			event_type = "???";
			break;
	}

	return event_type;
}

/* ----------
 * pgstat_get_wait_event() -
 *
 *	Return a string representing the current wait event, backend is
 *	waiting on.
 */
const char *
pgstat_get_wait_event(uint32 wait_event_info)
{
	uint32		classId;
	uint16		eventId;
	const char *event_name;

	/* report process as not waiting. */
	if (wait_event_info == 0)
		return NULL;

	classId = wait_event_info & 0xFF000000;
	eventId = wait_event_info & 0x0000FFFF;

	switch (classId)
	{
		case PG_WAIT_LWLOCK:
			event_name = GetLWLockIdentifier(classId, eventId);
			break;
		case PG_WAIT_LOCK:
			event_name = GetLockNameFromTagType(eventId);
			break;
		case PG_WAIT_BUFFER_PIN:
			event_name = "BufferPin";
			break;
		case PG_WAIT_ACTIVITY:
			{
				WaitEventActivity w = (WaitEventActivity) wait_event_info;

				event_name = pgstat_get_wait_activity(w);
				break;
			}
		case PG_WAIT_CLIENT:
			{
				WaitEventClient w = (WaitEventClient) wait_event_info;

				event_name = pgstat_get_wait_client(w);
				break;
			}
		case PG_WAIT_EXTENSION:
			event_name = "Extension";
			break;
		case PG_WAIT_IPC:
			{
				WaitEventIPC w = (WaitEventIPC) wait_event_info;

				event_name = pgstat_get_wait_ipc(w);
				break;
			}
		case PG_WAIT_TIMEOUT:
			{
				WaitEventTimeout w = (WaitEventTimeout) wait_event_info;

				event_name = pgstat_get_wait_timeout(w);
				break;
			}
		case PG_WAIT_IO:
			{
				WaitEventIO w = (WaitEventIO) wait_event_info;

				event_name = pgstat_get_wait_io(w);
				break;
			}
		default:
			event_name = "unknown wait event";
			break;
	}

	return event_name;
}

/* ----------
 * pgstat_get_wait_activity() -
 *
 * Convert WaitEventActivity to string.
 * ----------
 */
static const char *
pgstat_get_wait_activity(WaitEventActivity w)
{
	const char *event_name = "unknown wait event";

	switch (w)
	{
		case WAIT_EVENT_ARCHIVER_MAIN:
			event_name = "ArchiverMain";
			break;
		case WAIT_EVENT_AUTOVACUUM_MAIN:
			event_name = "AutoVacuumMain";
			break;
		case WAIT_EVENT_BGWRITER_HIBERNATE:
			event_name = "BgWriterHibernate";
			break;
		case WAIT_EVENT_BGWRITER_MAIN:
			event_name = "BgWriterMain";
			break;
		case WAIT_EVENT_CHECKPOINTER_MAIN:
			event_name = "CheckpointerMain";
			break;
		case WAIT_EVENT_LOGICAL_APPLY_MAIN:
			event_name = "LogicalApplyMain";
			break;
		case WAIT_EVENT_LOGICAL_LAUNCHER_MAIN:
			event_name = "LogicalLauncherMain";
			break;
		case WAIT_EVENT_RECOVERY_WAL_ALL:
			event_name = "RecoveryWalAll";
			break;
		case WAIT_EVENT_RECOVERY_WAL_STREAM:
			event_name = "RecoveryWalStream";
			break;
		case WAIT_EVENT_SYSLOGGER_MAIN:
			event_name = "SysLoggerMain";
			break;
		case WAIT_EVENT_WAL_RECEIVER_MAIN:
			event_name = "WalReceiverMain";
			break;
		case WAIT_EVENT_WAL_SENDER_MAIN:
			event_name = "WalSenderMain";
			break;
		case WAIT_EVENT_WAL_WRITER_MAIN:
			event_name = "WalWriterMain";
			break;
			/* no default case, so that compiler will warn */
	}

	return event_name;
}

/* ----------
 * pgstat_get_wait_client() -
 *
 * Convert WaitEventClient to string.
 * ----------
 */
static const char *
pgstat_get_wait_client(WaitEventClient w)
{
	const char *event_name = "unknown wait event";

	switch (w)
	{
		case WAIT_EVENT_CLIENT_READ:
			event_name = "ClientRead";
			break;
		case WAIT_EVENT_CLIENT_WRITE:
			event_name = "ClientWrite";
			break;
		case WAIT_EVENT_LIBPQWALRECEIVER_CONNECT:
			event_name = "LibPQWalReceiverConnect";
			break;
		case WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE:
			event_name = "LibPQWalReceiverReceive";
			break;
		case WAIT_EVENT_SSL_OPEN_SERVER:
			event_name = "SSLOpenServer";
			break;
		case WAIT_EVENT_WAL_RECEIVER_WAIT_START:
			event_name = "WalReceiverWaitStart";
			break;
		case WAIT_EVENT_WAL_SENDER_WAIT_WAL:
			event_name = "WalSenderWaitForWAL";
			break;
		case WAIT_EVENT_WAL_SENDER_WRITE_DATA:
			event_name = "WalSenderWriteData";
			break;
			/* no default case, so that compiler will warn */
	}

	return event_name;
}

/* ----------
 * pgstat_get_wait_ipc() -
 *
 * Convert WaitEventIPC to string.
 * ----------
 */
static const char *
pgstat_get_wait_ipc(WaitEventIPC w)
{
	const char *event_name = "unknown wait event";

	switch (w)
	{
		case WAIT_EVENT_BGWORKER_SHUTDOWN:
			event_name = "BgWorkerShutdown";
			break;
		case WAIT_EVENT_BGWORKER_STARTUP:
			event_name = "BgWorkerStartup";
			break;
		case WAIT_EVENT_BTREE_PAGE:
			event_name = "BtreePage";
			break;
		case WAIT_EVENT_CHECKPOINT_DONE:
			event_name = "CheckpointDone";
			break;
		case WAIT_EVENT_CHECKPOINT_START:
			event_name = "CheckpointStart";
			break;
		case WAIT_EVENT_CLOG_GROUP_UPDATE:
			event_name = "ClogGroupUpdate";
			break;
		case WAIT_EVENT_EXECUTE_GATHER:
			event_name = "ExecuteGather";
			break;
		case WAIT_EVENT_HASH_BATCH_ALLOCATING:
			event_name = "Hash/Batch/Allocating";
			break;
		case WAIT_EVENT_HASH_BATCH_ELECTING:
			event_name = "Hash/Batch/Electing";
			break;
		case WAIT_EVENT_HASH_BATCH_LOADING:
			event_name = "Hash/Batch/Loading";
			break;
		case WAIT_EVENT_HASH_BUILD_ALLOCATING:
			event_name = "Hash/Build/Allocating";
			break;
		case WAIT_EVENT_HASH_BUILD_ELECTING:
			event_name = "Hash/Build/Electing";
			break;
		case WAIT_EVENT_HASH_BUILD_HASHING_INNER:
			event_name = "Hash/Build/HashingInner";
			break;
		case WAIT_EVENT_HASH_BUILD_HASHING_OUTER:
			event_name = "Hash/Build/HashingOuter";
			break;
		case WAIT_EVENT_HASH_GROW_BATCHES_ALLOCATING:
			event_name = "Hash/GrowBatches/Allocating";
			break;
		case WAIT_EVENT_HASH_GROW_BATCHES_DECIDING:
			event_name = "Hash/GrowBatches/Deciding";
			break;
		case WAIT_EVENT_HASH_GROW_BATCHES_ELECTING:
			event_name = "Hash/GrowBatches/Electing";
			break;
		case WAIT_EVENT_HASH_GROW_BATCHES_FINISHING:
			event_name = "Hash/GrowBatches/Finishing";
			break;
		case WAIT_EVENT_HASH_GROW_BATCHES_REPARTITIONING:
			event_name = "Hash/GrowBatches/Repartitioning";
			break;
		case WAIT_EVENT_HASH_GROW_BUCKETS_ALLOCATING:
			event_name = "Hash/GrowBuckets/Allocating";
			break;
		case WAIT_EVENT_HASH_GROW_BUCKETS_ELECTING:
			event_name = "Hash/GrowBuckets/Electing";
			break;
		case WAIT_EVENT_HASH_GROW_BUCKETS_REINSERTING:
			event_name = "Hash/GrowBuckets/Reinserting";
			break;
		case WAIT_EVENT_LOGICAL_SYNC_DATA:
			event_name = "LogicalSyncData";
			break;
		case WAIT_EVENT_LOGICAL_SYNC_STATE_CHANGE:
			event_name = "LogicalSyncStateChange";
			break;
		case WAIT_EVENT_MQ_INTERNAL:
			event_name = "MessageQueueInternal";
			break;
		case WAIT_EVENT_MQ_PUT_MESSAGE:
			event_name = "MessageQueuePutMessage";
			break;
		case WAIT_EVENT_MQ_RECEIVE:
			event_name = "MessageQueueReceive";
			break;
		case WAIT_EVENT_MQ_SEND:
			event_name = "MessageQueueSend";
			break;
		case WAIT_EVENT_PARALLEL_BITMAP_SCAN:
			event_name = "ParallelBitmapScan";
			break;
		case WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN:
			event_name = "ParallelCreateIndexScan";
			break;
		case WAIT_EVENT_PARALLEL_FINISH:
			event_name = "ParallelFinish";
			break;
		case WAIT_EVENT_PROCARRAY_GROUP_UPDATE:
			event_name = "ProcArrayGroupUpdate";
			break;
		case WAIT_EVENT_PROMOTE:
			event_name = "Promote";
			break;
		case WAIT_EVENT_REPLICATION_ORIGIN_DROP:
			event_name = "ReplicationOriginDrop";
			break;
		case WAIT_EVENT_REPLICATION_SLOT_DROP:
			event_name = "ReplicationSlotDrop";
			break;
		case WAIT_EVENT_SAFE_SNAPSHOT:
			event_name = "SafeSnapshot";
			break;
		case WAIT_EVENT_SYNC_REP:
			event_name = "SyncRep";
			break;
			/* no default case, so that compiler will warn */
	}

	return event_name;
}

/* ----------
 * pgstat_get_wait_timeout() -
 *
 * Convert WaitEventTimeout to string.
 * ----------
 */
static const char *
pgstat_get_wait_timeout(WaitEventTimeout w)
{
	const char *event_name = "unknown wait event";

	switch (w)
	{
		case WAIT_EVENT_BASE_BACKUP_THROTTLE:
			event_name = "BaseBackupThrottle";
			break;
		case WAIT_EVENT_PG_SLEEP:
			event_name = "PgSleep";
			break;
		case WAIT_EVENT_RECOVERY_APPLY_DELAY:
			event_name = "RecoveryApplyDelay";
			break;
			/* no default case, so that compiler will warn */
	}

	return event_name;
}

/* ----------
 * pgstat_get_wait_io() -
 *
 * Convert WaitEventIO to string.
 * ----------
 */
static const char *
pgstat_get_wait_io(WaitEventIO w)
{
	const char *event_name = "unknown wait event";

	switch (w)
	{
		case WAIT_EVENT_BUFFILE_READ:
			event_name = "BufFileRead";
			break;
		case WAIT_EVENT_BUFFILE_WRITE:
			event_name = "BufFileWrite";
			break;
		case WAIT_EVENT_CONTROL_FILE_READ:
			event_name = "ControlFileRead";
			break;
		case WAIT_EVENT_CONTROL_FILE_SYNC:
			event_name = "ControlFileSync";
			break;
		case WAIT_EVENT_CONTROL_FILE_SYNC_UPDATE:
			event_name = "ControlFileSyncUpdate";
			break;
		case WAIT_EVENT_CONTROL_FILE_WRITE:
			event_name = "ControlFileWrite";
			break;
		case WAIT_EVENT_CONTROL_FILE_WRITE_UPDATE:
			event_name = "ControlFileWriteUpdate";
			break;
		case WAIT_EVENT_COPY_FILE_READ:
			event_name = "CopyFileRead";
			break;
		case WAIT_EVENT_COPY_FILE_WRITE:
			event_name = "CopyFileWrite";
			break;
		case WAIT_EVENT_DATA_FILE_EXTEND:
			event_name = "DataFileExtend";
			break;
		case WAIT_EVENT_DATA_FILE_FLUSH:
			event_name = "DataFileFlush";
			break;
		case WAIT_EVENT_DATA_FILE_IMMEDIATE_SYNC:
			event_name = "DataFileImmediateSync";
			break;
		case WAIT_EVENT_DATA_FILE_PREFETCH:
			event_name = "DataFilePrefetch";
			break;
		case WAIT_EVENT_DATA_FILE_READ:
			event_name = "DataFileRead";
			break;
		case WAIT_EVENT_DATA_FILE_SYNC:
			event_name = "DataFileSync";
			break;
		case WAIT_EVENT_DATA_FILE_TRUNCATE:
			event_name = "DataFileTruncate";
			break;
		case WAIT_EVENT_DATA_FILE_WRITE:
			event_name = "DataFileWrite";
			break;
		case WAIT_EVENT_DSM_FILL_ZERO_WRITE:
			event_name = "DSMFillZeroWrite";
			break;
		case WAIT_EVENT_LOCK_FILE_ADDTODATADIR_READ:
			event_name = "LockFileAddToDataDirRead";
			break;
		case WAIT_EVENT_LOCK_FILE_ADDTODATADIR_SYNC:
			event_name = "LockFileAddToDataDirSync";
			break;
		case WAIT_EVENT_LOCK_FILE_ADDTODATADIR_WRITE:
			event_name = "LockFileAddToDataDirWrite";
			break;
		case WAIT_EVENT_LOCK_FILE_CREATE_READ:
			event_name = "LockFileCreateRead";
			break;
		case WAIT_EVENT_LOCK_FILE_CREATE_SYNC:
			event_name = "LockFileCreateSync";
			break;
		case WAIT_EVENT_LOCK_FILE_CREATE_WRITE:
			event_name = "LockFileCreateWrite";
			break;
		case WAIT_EVENT_LOCK_FILE_RECHECKDATADIR_READ:
			event_name = "LockFileReCheckDataDirRead";
			break;
		case WAIT_EVENT_LOGICAL_REWRITE_CHECKPOINT_SYNC:
			event_name = "LogicalRewriteCheckpointSync";
			break;
		case WAIT_EVENT_LOGICAL_REWRITE_MAPPING_SYNC:
			event_name = "LogicalRewriteMappingSync";
			break;
		case WAIT_EVENT_LOGICAL_REWRITE_MAPPING_WRITE:
			event_name = "LogicalRewriteMappingWrite";
			break;
		case WAIT_EVENT_LOGICAL_REWRITE_SYNC:
			event_name = "LogicalRewriteSync";
			break;
		case WAIT_EVENT_LOGICAL_REWRITE_TRUNCATE:
			event_name = "LogicalRewriteTruncate";
			break;
		case WAIT_EVENT_LOGICAL_REWRITE_WRITE:
			event_name = "LogicalRewriteWrite";
			break;
		case WAIT_EVENT_RELATION_MAP_READ:
			event_name = "RelationMapRead";
			break;
		case WAIT_EVENT_RELATION_MAP_SYNC:
			event_name = "RelationMapSync";
			break;
		case WAIT_EVENT_RELATION_MAP_WRITE:
			event_name = "RelationMapWrite";
			break;
		case WAIT_EVENT_REORDER_BUFFER_READ:
			event_name = "ReorderBufferRead";
			break;
		case WAIT_EVENT_REORDER_BUFFER_WRITE:
			event_name = "ReorderBufferWrite";
			break;
		case WAIT_EVENT_REORDER_LOGICAL_MAPPING_READ:
			event_name = "ReorderLogicalMappingRead";
			break;
		case WAIT_EVENT_REPLICATION_SLOT_READ:
			event_name = "ReplicationSlotRead";
			break;
		case WAIT_EVENT_REPLICATION_SLOT_RESTORE_SYNC:
			event_name = "ReplicationSlotRestoreSync";
			break;
		case WAIT_EVENT_REPLICATION_SLOT_SYNC:
			event_name = "ReplicationSlotSync";
			break;
		case WAIT_EVENT_REPLICATION_SLOT_WRITE:
			event_name = "ReplicationSlotWrite";
			break;
		case WAIT_EVENT_SLRU_FLUSH_SYNC:
			event_name = "SLRUFlushSync";
			break;
		case WAIT_EVENT_SLRU_READ:
			event_name = "SLRURead";
			break;
		case WAIT_EVENT_SLRU_SYNC:
			event_name = "SLRUSync";
			break;
		case WAIT_EVENT_SLRU_WRITE:
			event_name = "SLRUWrite";
			break;
		case WAIT_EVENT_SNAPBUILD_READ:
			event_name = "SnapbuildRead";
			break;
		case WAIT_EVENT_SNAPBUILD_SYNC:
			event_name = "SnapbuildSync";
			break;
		case WAIT_EVENT_SNAPBUILD_WRITE:
			event_name = "SnapbuildWrite";
			break;
		case WAIT_EVENT_TIMELINE_HISTORY_FILE_SYNC:
			event_name = "TimelineHistoryFileSync";
			break;
		case WAIT_EVENT_TIMELINE_HISTORY_FILE_WRITE:
			event_name = "TimelineHistoryFileWrite";
			break;
		case WAIT_EVENT_TIMELINE_HISTORY_READ:
			event_name = "TimelineHistoryRead";
			break;
		case WAIT_EVENT_TIMELINE_HISTORY_SYNC:
			event_name = "TimelineHistorySync";
			break;
		case WAIT_EVENT_TIMELINE_HISTORY_WRITE:
			event_name = "TimelineHistoryWrite";
			break;
		case WAIT_EVENT_TWOPHASE_FILE_READ:
			event_name = "TwophaseFileRead";
			break;
		case WAIT_EVENT_TWOPHASE_FILE_SYNC:
			event_name = "TwophaseFileSync";
			break;
		case WAIT_EVENT_TWOPHASE_FILE_WRITE:
			event_name = "TwophaseFileWrite";
			break;
		case WAIT_EVENT_WALSENDER_TIMELINE_HISTORY_READ:
			event_name = "WALSenderTimelineHistoryRead";
			break;
		case WAIT_EVENT_WAL_BOOTSTRAP_SYNC:
			event_name = "WALBootstrapSync";
			break;
		case WAIT_EVENT_WAL_BOOTSTRAP_WRITE:
			event_name = "WALBootstrapWrite";
			break;
		case WAIT_EVENT_WAL_COPY_READ:
			event_name = "WALCopyRead";
			break;
		case WAIT_EVENT_WAL_COPY_SYNC:
			event_name = "WALCopySync";
			break;
		case WAIT_EVENT_WAL_COPY_WRITE:
			event_name = "WALCopyWrite";
			break;
		case WAIT_EVENT_WAL_INIT_SYNC:
			event_name = "WALInitSync";
			break;
		case WAIT_EVENT_WAL_INIT_WRITE:
			event_name = "WALInitWrite";
			break;
		case WAIT_EVENT_WAL_READ:
			event_name = "WALRead";
			break;
		case WAIT_EVENT_WAL_SYNC:
			event_name = "WALSync";
			break;
		case WAIT_EVENT_WAL_SYNC_METHOD_ASSIGN:
			event_name = "WALSyncMethodAssign";
			break;
		case WAIT_EVENT_WAL_WRITE:
			event_name = "WALWrite";
			break;

			/* no default case, so that compiler will warn */
	}

	return event_name;
}


/* ----------
 * pgstat_get_backend_current_activity() -
 *
 *	Return a string representing the current activity of the backend with
 *	the specified PID.  This looks directly at the BackendStatusArray,
 *	and so will provide current information regardless of the age of our
 *	transaction's snapshot of the status array.
 *
 *	It is the caller's responsibility to invoke this only for backends whose
 *	state is expected to remain stable while the result is in use.  The
 *	only current use is in deadlock reporting, where we can expect that
 *	the target backend is blocked on a lock.  (There are corner cases
 *	where the target's wait could get aborted while we are looking at it,
 *	but the very worst consequence is to return a pointer to a string
 *	that's been changed, so we won't worry too much.)
 *
 *	Note: return strings for special cases match pg_stat_get_backend_activity.
 * ----------
 */
const char *
pgstat_get_backend_current_activity(int pid, bool checkUser)
{
	PgBackendStatus *beentry;
	int			i;

	beentry = BackendStatusArray;
	for (i = 1; i <= MaxBackends; i++)
	{
		/*
		 * Although we expect the target backend's entry to be stable, that
		 * doesn't imply that anyone else's is.  To avoid identifying the
		 * wrong backend, while we check for a match to the desired PID we
		 * must follow the protocol of retrying if st_changecount changes
		 * while we examine the entry, or if it's odd.  (This might be
		 * unnecessary, since fetching or storing an int is almost certainly
		 * atomic, but let's play it safe.)  We use a volatile pointer here to
		 * ensure the compiler doesn't try to get cute.
		 */
		volatile PgBackendStatus *vbeentry = beentry;
		bool		found;

		for (;;)
		{
			int			before_changecount;
			int			after_changecount;

			pgstat_save_changecount_before(vbeentry, before_changecount);

			found = (vbeentry->st_procpid == pid);

			pgstat_save_changecount_after(vbeentry, after_changecount);

			if (before_changecount == after_changecount &&
				(before_changecount & 1) == 0)
				break;

			/* Make sure we can break out of loop if stuck... */
			CHECK_FOR_INTERRUPTS();
		}

		if (found)
		{
			/* Now it is safe to use the non-volatile pointer */
			if (checkUser && !superuser() && beentry->st_userid != GetUserId())
				return "<insufficient privilege>";
			else if (*(beentry->st_activity_raw) == '\0')
				return "<command string not enabled>";
			else
			{
				/* this'll leak a bit of memory, but that seems acceptable */
				return pgstat_clip_activity(beentry->st_activity_raw);
			}
		}

		beentry++;
	}

	/* If we get here, caller is in error ... */
	return "<backend information not available>";
}

/* ----------
 * pgstat_get_crashed_backend_activity() -
 *
 *	Return a string representing the current activity of the backend with
 *	the specified PID.  Like the function above, but reads shared memory with
 *	the expectation that it may be corrupt.  On success, copy the string
 *	into the "buffer" argument and return that pointer.  On failure,
 *	return NULL.
 *
 *	This function is only intended to be used by the postmaster to report the
 *	query that crashed a backend.  In particular, no attempt is made to
 *	follow the correct concurrency protocol when accessing the
 *	BackendStatusArray.  But that's OK, in the worst case we'll return a
 *	corrupted message.  We also must take care not to trip on ereport(ERROR).
 * ----------
 */
const char *
pgstat_get_crashed_backend_activity(int pid, char *buffer, int buflen)
{
	volatile PgBackendStatus *beentry;
	int			i;

	beentry = BackendStatusArray;

	/*
	 * We probably shouldn't get here before shared memory has been set up,
	 * but be safe.
	 */
	if (beentry == NULL || BackendActivityBuffer == NULL)
		return NULL;

	for (i = 1; i <= MaxBackends; i++)
	{
		if (beentry->st_procpid == pid)
		{
			/* Read pointer just once, so it can't change after validation */
			const char *activity = beentry->st_activity_raw;
			const char *activity_last;

			/*
			 * We mustn't access activity string before we verify that it
			 * falls within the BackendActivityBuffer. To make sure that the
			 * entire string including its ending is contained within the
			 * buffer, subtract one activity length from the buffer size.
			 */
			activity_last = BackendActivityBuffer + BackendActivityBufferSize
				- pgstat_track_activity_query_size;

			if (activity < BackendActivityBuffer ||
				activity > activity_last)
				return NULL;

			/* If no string available, no point in a report */
			if (activity[0] == '\0')
				return NULL;

			/*
			 * Copy only ASCII-safe characters so we don't run into encoding
			 * problems when reporting the message; and be sure not to run off
			 * the end of memory.  As only ASCII characters are reported, it
			 * doesn't seem necessary to perform multibyte aware clipping.
			 */
			ascii_safe_strlcpy(buffer, activity,
							   Min(buflen, pgstat_track_activity_query_size));

			return buffer;
		}

		beentry++;
	}

	/* PID not found */
	return NULL;
}

const char *
pgstat_get_backend_desc(BackendType backendType)
{
	const char *backendDesc = "unknown process type";

	switch (backendType)
	{
		case B_ARCHIVER:
			backendDesc = "archiver";
			break;
		case B_AUTOVAC_LAUNCHER:
			backendDesc = "autovacuum launcher";
			break;
		case B_AUTOVAC_WORKER:
			backendDesc = "autovacuum worker";
			break;
		case B_BACKEND:
			backendDesc = "client backend";
			break;
		case B_BG_WORKER:
			backendDesc = "background worker";
			break;
		case B_BG_WRITER:
			backendDesc = "background writer";
			break;
		case B_CHECKPOINTER:
			backendDesc = "checkpointer";
			break;
		case B_STARTUP:
			backendDesc = "startup";
			break;
		case B_WAL_RECEIVER:
			backendDesc = "walreceiver";
			break;
		case B_WAL_SENDER:
			backendDesc = "walsender";
			break;
		case B_WAL_WRITER:
			backendDesc = "walwriter";
			break;
	}

	return backendDesc;
}

/* ------------------------------------------------------------
 * Local support functions follow
 * ------------------------------------------------------------
 */

/* ----------
 * pgstat_send_archiver() -
 *
 *		Report archiver statistics
 * ----------
 */
void
pgstat_send_archiver(const char *xlog, bool failed)
{
	LWLockAcquire(StatsLock, LW_EXCLUSIVE);
	if (failed)
	{
		/* Failed archival attempt */
		++shared_archiverStats->failed_count;
		memcpy(shared_archiverStats->last_failed_wal, xlog,
			   sizeof(shared_archiverStats->last_failed_wal));
		shared_archiverStats->last_failed_timestamp = GetCurrentTimestamp();
	}
	else
	{
		/* Successful archival operation */
		++shared_archiverStats->archived_count;
		memcpy(shared_archiverStats->last_archived_wal, xlog,
			   sizeof(shared_archiverStats->last_archived_wal));
		shared_archiverStats->last_archived_timestamp = GetCurrentTimestamp();
	}
	LWLockRelease(StatsLock);
}

/* ----------
 * pgstat_send_bgwriter() -
 *
 *		Report bgwriter statistics
 * ----------
 */
void
pgstat_send_bgwriter(void)
{
	/* We assume this initializes to zeroes */
	static const PgStat_MsgBgWriter all_zeroes;

	PgStat_MsgBgWriter *s = &BgWriterStats;

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid sending a completely empty message to the stats
	 * collector.
	 */
	if (memcmp(&BgWriterStats, &all_zeroes, sizeof(PgStat_MsgBgWriter)) == 0)
		return;

	LWLockAcquire(StatsLock, LW_EXCLUSIVE);
	shared_globalStats->timed_checkpoints += s->m_timed_checkpoints;
	shared_globalStats->requested_checkpoints += s->m_requested_checkpoints;
	shared_globalStats->checkpoint_write_time += s->m_checkpoint_write_time;
	shared_globalStats->checkpoint_sync_time += s->m_checkpoint_sync_time;
	shared_globalStats->buf_written_checkpoints += s->m_buf_written_checkpoints;
	shared_globalStats->buf_written_clean += s->m_buf_written_clean;
	shared_globalStats->maxwritten_clean += s->m_maxwritten_clean;
	shared_globalStats->buf_written_backend += s->m_buf_written_backend;
	shared_globalStats->buf_fsync_backend += s->m_buf_fsync_backend;
	shared_globalStats->buf_alloc += s->m_buf_alloc;
	LWLockRelease(StatsLock);

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&BgWriterStats, 0, sizeof(BgWriterStats));
}

/*
 * Pin and Unpin dbentry.
 *
 * To keep less memory usage, and for speed, counters are by recreation of
 * dshash instead of removing entries one-by-one keeping whole-dshash lock. On
 * the other hand dshash cannot be destroyed until all referrers have gone. As
 * the result, other backend may be kept waiting the counter reset for not a
 * short time. We isolate the hashes under destruction as another generation,
 * which means no longer used but cannot be removed yet.

 * When we start accessing hashes on a dbentry, call pin_hashes() and acquire
 * the current "generation". Unlock removes the older generation's hashes when
 * all refers have gone.
 */
static int
pin_hashes(PgStat_StatDBEntry *dbentry)
{
	int	generation;

	LWLockAcquire(&dbentry->lock, LW_EXCLUSIVE);
	dbentry->refcnt++;
	generation = dbentry->generation;
	LWLockRelease(&dbentry->lock);

	dshash_release_lock(pgStatDBHash, dbentry);

	return generation;
}

/*
 * Unpin hashes in dbentry. If given generation is isolated, destroy it after
 * all referrers has gone. Otherwise just decrease reference count then return.
 */
static void
unpin_hashes(PgStat_StatDBEntry *dbentry, int generation)
{
	dshash_table *tables;
	dshash_table *funcs = NULL;

	LWLockAcquire(&dbentry->lock, LW_EXCLUSIVE);

	/* using current generation, just decrease refcount */
	if (dbentry->generation == generation)
	{
		dbentry->refcnt--;
		LWLockRelease(&dbentry->lock);
		return;
	}

	/*
	 * It is isolated, waiting for all referrers to end.
	 */
	Assert(dbentry->generation == generation + 1);

	if (--dbentry->prev_refcnt > 0)
	{
		LWLockRelease(&dbentry->lock);
		return;
	}

	/* no referrer remains, remove the hashes */
	tables = dshash_attach(area, &dsh_tblparams, dbentry->prev_tables, 0);
	if (dbentry->prev_functions != DSM_HANDLE_INVALID)
		funcs = dshash_attach(area, &dsh_funcparams,
							  dbentry->prev_functions, 0);

	dbentry->prev_tables = DSM_HANDLE_INVALID;
	dbentry->prev_functions = DSM_HANDLE_INVALID;

	/* release the entry immediately */
	LWLockRelease(&dbentry->lock);

	dshash_destroy(tables);
	if (funcs)
		dshash_destroy(funcs);

	return;
}

/*
 * attach and return the specified generation of table hash
 * Returns NULL on lock failure.
 */
static dshash_table *
attach_table_hash(PgStat_StatDBEntry *dbent, int gen)
{
	dshash_table *ret;

	LWLockAcquire(&dbent->lock, LW_EXCLUSIVE);

	if (dbent->generation == gen)
		ret = dshash_attach(area, &dsh_tblparams, dbent->tables, 0);
	else
	{
		Assert (dbent->generation == gen + 1);
		Assert (dbent->prev_tables != DSM_HANDLE_INVALID);
		ret = dshash_attach(area, &dsh_tblparams, dbent->prev_tables, 0);
	}
	LWLockRelease(&dbent->lock);

	return ret;
}

/* attach and return the specified generation of function hash */
static dshash_table *
attach_function_hash(PgStat_StatDBEntry *dbent, int gen)
{
	dshash_table *ret = NULL;

	LWLockAcquire(&dbent->lock, LW_EXCLUSIVE);

	if (dbent->generation == gen)
	{
		if (dbent->functions == DSM_HANDLE_INVALID)
		{
			dshash_table *funchash =
				dshash_create(area, &dsh_funcparams, 0);
			dbent->functions = dshash_get_hash_table_handle(funchash);

			ret = funchash;
		}
		else
			ret =  dshash_attach(area, &dsh_funcparams, dbent->functions, 0);
	}
	/* don't bother creating useless hash */

	LWLockRelease(&dbent->lock);

	return  ret;
}

static void
init_dbentry(PgStat_StatDBEntry *dbentry)
{
	LWLockInitialize(&dbentry->lock, LWTRANCHE_STATS);
	dbentry->generation = 0;
	dbentry->refcnt = 0;
	dbentry->prev_refcnt = 0;
	dbentry->tables = DSM_HANDLE_INVALID;
	dbentry->prev_tables = DSM_HANDLE_INVALID;
	dbentry->functions = DSM_HANDLE_INVALID;
	dbentry->prev_functions = DSM_HANDLE_INVALID;
}

/*
 * Subroutine to reset stats in a shared database entry
 *
 * All counters are reset. Tables and functions dshashes are destroyed.  If
 * any backend is pinning this dbentry, the current dshashes are stashed out to
 * the previous "generation" to wait for all accessors gone. If the previous
 * generation is already occupied, the current dshashes are so fresh that they
 * doesn't need to be cleared.
 */
static void
reset_dbentry_counters(PgStat_StatDBEntry *dbentry)
{
	LWLockAcquire(&dbentry->lock, LW_EXCLUSIVE);

	dbentry->n_xact_commit = 0;
	dbentry->n_xact_rollback = 0;
	dbentry->n_blocks_fetched = 0;
	dbentry->n_blocks_hit = 0;
	dbentry->n_tuples_returned = 0;
	dbentry->n_tuples_fetched = 0;
	dbentry->n_tuples_inserted = 0;
	dbentry->n_tuples_updated = 0;
	dbentry->n_tuples_deleted = 0;
	dbentry->last_autovac_time = 0;
	dbentry->n_conflict_tablespace = 0;
	dbentry->n_conflict_lock = 0;
	dbentry->n_conflict_snapshot = 0;
	dbentry->n_conflict_bufferpin = 0;
	dbentry->n_conflict_startup_deadlock = 0;
	dbentry->n_temp_files = 0;
	dbentry->n_temp_bytes = 0;
	dbentry->n_deadlocks = 0;
	dbentry->n_checksum_failures = 0;
	dbentry->n_block_read_time = 0;
	dbentry->n_block_write_time = 0;

	if (dbentry->refcnt == 0)
	{
		/*
		 * No one is referring to the current hash. Removing individual
		 * entries in dshash is very costly so just destroy it.  If someone
		 * pined this entry just after, pin_hashes returns the current
		 * generation and attach waits for the following LWLock.
		 */
		dshash_table *tbl;

		if (dbentry->tables != DSM_HANDLE_INVALID)
		{
			tbl = dshash_attach(area, &dsh_tblparams, dbentry->tables, 0);
			dshash_destroy(tbl);
			dbentry->tables = DSM_HANDLE_INVALID;
		}
		if (dbentry->functions != DSM_HANDLE_INVALID)
		{
			tbl = dshash_attach(area, &dsh_funcparams, dbentry->functions, 0);
			dshash_destroy(tbl);
			dbentry->functions = DSM_HANDLE_INVALID;
		}
	}
	else if (dbentry->prev_refcnt == 0)
	{
		/*
		 * Someone is still referring to the current hash and previous slot is
		 * vacant. Stash out the current hash to the previous slot.
		 */
		dbentry->prev_refcnt = dbentry->refcnt;
		dbentry->prev_tables = dbentry->tables;
		dbentry->prev_functions = dbentry->functions;
		dbentry->refcnt = 0;
		dbentry->tables = DSM_HANDLE_INVALID;
		dbentry->functions = DSM_HANDLE_INVALID;
		dbentry->generation++;
	}
	else
	{
		Assert(dbentry->prev_refcnt > 0 && dbentry->refcnt > 0);
		/*
		 * If we get here, we just have got another reset request and the old
		 * hashes are waiting to all referrers to release. It must be quite a
		 * short time so we can just ignore this request.
		 */
	}

	/* Create new table hash if not exists */
	if (dbentry->tables == DSM_HANDLE_INVALID)
	{
		dshash_table *tbl = dshash_create(area, &dsh_tblparams, 0);
		dbentry->tables = dshash_get_hash_table_handle(tbl);
		dshash_detach(tbl);
	}

	/* Recreate now if needed. */
	if (dbentry->functions == DSM_HANDLE_INVALID &&
		pgstat_track_functions != TRACK_FUNC_OFF)
	{
		dshash_table *tbl = dshash_create(area, &dsh_funcparams, 0);
		dbentry->functions = dshash_get_hash_table_handle(tbl);
		dshash_detach(tbl);
	}

	dbentry->stat_reset_timestamp = GetCurrentTimestamp();

	LWLockRelease(&dbentry->lock);
}

/*
 * Create the filename for a DB stat file; filename is the output buffer, of
 * length len.
 */
static void
get_dbstat_filename(bool tempname, Oid databaseid, char *filename, int len)
{
	int			printed;

	/* NB -- pgstat_reset_remove_files knows about the pattern this uses */
	printed = snprintf(filename, len, "%s/db_%u.%s",
					   PGSTAT_STAT_PERMANENT_DIRECTORY,
					   databaseid,
					   tempname ? "tmp" : "stat");
	if (printed >= len)
		elog(ERROR, "overlength pgstat path");
}

/* ----------
 * pgstat_write_statsfiles() -
 *		Write the global statistics file, as well as DB files.
 * ----------
 */
void
pgstat_write_statsfiles(void)
{
	dshash_seq_status hstat;
	PgStat_StatDBEntry *dbentry;
	FILE	   *fpout;
	int32		format_id;
	const char *tmpfile = PGSTAT_STAT_PERMANENT_TMPFILE;
	const char *statfile = PGSTAT_STAT_PERMANENT_FILENAME;
	int			rc;

	/* should be called from postmaster  */
	Assert(!IsUnderPostmaster);

	elog(DEBUG2, "writing stats file \"%s\"", statfile);

	/*
	 * Open the statistics temp file to write out the current values.
	 */
	fpout = AllocateFile(tmpfile, PG_BINARY_W);
	if (fpout == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open temporary statistics file \"%s\": %m",
						tmpfile)));
		return;
	}

	/*
	 * Set the timestamp of the stats file.
	 */
	shared_globalStats->stats_timestamp = GetCurrentTimestamp();

	/*
	 * Write the file header --- currently just a format ID.
	 */
	format_id = PGSTAT_FILE_FORMAT_ID;
	rc = fwrite(&format_id, sizeof(format_id), 1, fpout);
	(void) rc;					/* we'll check for error with ferror */

	/*
	 * Write global stats struct
	 */
	rc = fwrite(shared_globalStats, sizeof(*shared_globalStats), 1, fpout);
	(void) rc;					/* we'll check for error with ferror */

	/*
	 * Write archiver stats struct
	 */
	rc = fwrite(shared_archiverStats, sizeof(*shared_archiverStats), 1, fpout);
	(void) rc;					/* we'll check for error with ferror */

	/*
	 * Walk through the database table.
	 */
	dshash_seq_init(&hstat, pgStatDBHash, false, false);
	while ((dbentry = (PgStat_StatDBEntry *) dshash_seq_next(&hstat)) != NULL)
	{
		/*
		 * Write out the table and function stats for this DB into the
		 * appropriate per-DB stat file, if required.
		 */
		/* Make DB's timestamp consistent with the global stats */
		dbentry->stats_timestamp = shared_globalStats->stats_timestamp;

		pgstat_write_pgStatDBHashfile(dbentry);

		/*
		 * Write out the DB entry. We don't write the tables or functions
		 * pointers, since they're of no use to any other process.
		 */
		fputc('D', fpout);
		rc = fwrite(dbentry,
					offsetof(PgStat_StatDBEntry, generation), 1, fpout);
		(void) rc;				/* we'll check for error with ferror */
	}

	/*
	 * No more output to be done. Close the temp file and replace the old
	 * pgstat.stat with it.  The ferror() check replaces testing for error
	 * after each individual fputc or fwrite above.
	 */
	fputc('E', fpout);

	if (ferror(fpout))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write temporary statistics file \"%s\": %m",
						tmpfile)));
		FreeFile(fpout);
		unlink(tmpfile);
	}
	else if (FreeFile(fpout) < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not close temporary statistics file \"%s\": %m",
						tmpfile)));
		unlink(tmpfile);
	}
	else if (rename(tmpfile, statfile) < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename temporary statistics file \"%s\" to \"%s\": %m",
						tmpfile, statfile)));
		unlink(tmpfile);
	}
}

/* ----------
 * pgstat_write_pgStatDBHashfile() -
 *		Write the stat file for a single database.
 * ----------
 */
static void
pgstat_write_pgStatDBHashfile(PgStat_StatDBEntry *dbentry)
{
	dshash_seq_status tstat;
	dshash_seq_status fstat;
	PgStat_StatTabEntry *tabentry;
	PgStat_StatFuncEntry *funcentry;
	FILE	   *fpout;
	int32		format_id;
	Oid			dbid = dbentry->databaseid;
	int			rc;
	char		tmpfile[MAXPGPATH];
	char		statfile[MAXPGPATH];
	dshash_table *tbl;

	get_dbstat_filename(true, dbid, tmpfile, MAXPGPATH);
	get_dbstat_filename(false, dbid, statfile, MAXPGPATH);

	elog(DEBUG2, "writing stats file \"%s\"", statfile);

	/*
	 * Open the statistics temp file to write out the current values.
	 */
	fpout = AllocateFile(tmpfile, PG_BINARY_W);
	if (fpout == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open temporary statistics file \"%s\": %m",
						tmpfile)));
		return;
	}

	/*
	 * Write the file header --- currently just a format ID.
	 */
	format_id = PGSTAT_FILE_FORMAT_ID;
	rc = fwrite(&format_id, sizeof(format_id), 1, fpout);
	(void) rc;					/* we'll check for error with ferror */

	/*
	 * Walk through the database's access stats per table.
	 */
	tbl = dshash_attach(area, &dsh_tblparams, dbentry->tables, 0);
	dshash_seq_init(&tstat, tbl, false, false);
	while ((tabentry = (PgStat_StatTabEntry *) dshash_seq_next(&tstat)) != NULL)
	{
		fputc('T', fpout);
		rc = fwrite(tabentry, sizeof(PgStat_StatTabEntry), 1, fpout);
		(void) rc;				/* we'll check for error with ferror */
	}
	dshash_detach(tbl);

	/*
	 * Walk through the database's function stats table.
	 */
	if (dbentry->functions != DSM_HANDLE_INVALID)
	{
		tbl = dshash_attach(area, &dsh_funcparams, dbentry->functions, 0);
		dshash_seq_init(&fstat, tbl, false, false);
		while ((funcentry = (PgStat_StatFuncEntry *) dshash_seq_next(&fstat)) != NULL)
		{
			fputc('F', fpout);
			rc = fwrite(funcentry, sizeof(PgStat_StatFuncEntry), 1, fpout);
			(void) rc;				/* we'll check for error with ferror */
		}
		dshash_detach(tbl);
	}

	/*
	 * No more output to be done. Close the temp file and replace the old
	 * pgstat.stat with it.  The ferror() check replaces testing for error
	 * after each individual fputc or fwrite above.
	 */
	fputc('E', fpout);

	if (ferror(fpout))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write temporary statistics file \"%s\": %m",
						tmpfile)));
		FreeFile(fpout);
		unlink(tmpfile);
	}
	else if (FreeFile(fpout) < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not close temporary statistics file \"%s\": %m",
						tmpfile)));
		unlink(tmpfile);
	}
	else if (rename(tmpfile, statfile) < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename temporary statistics file \"%s\" to \"%s\": %m",
						tmpfile, statfile)));
		unlink(tmpfile);
	}
}

/* ----------
 * pgstat_read_statsfiles() -
 *
 *	Reads in existing statistics collector files into the shared stats hash.
 *
 * ----------
 */
void
pgstat_read_statsfiles(void)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatDBEntry dbbuf;
	FILE	   *fpin;
	int32		format_id;
	bool		found;
	const char *statfile = PGSTAT_STAT_PERMANENT_FILENAME;

	/* should be called from postmaster  */
	Assert(!IsUnderPostmaster);

	/*
	 * local cache lives in pgSharedStatsContext.
	 */
	pgstat_setup_memcxt();

	/*
	 * Create the DB hashtable and global stats area. No lock is needed since
	 * we're alone now.
	 */
	pgstat_create_shared_stats();

	/*
	 * Set the current timestamp (will be kept only in case we can't load an
	 * existing statsfile).
	 */
	shared_globalStats->stat_reset_timestamp = GetCurrentTimestamp();
	shared_archiverStats->stat_reset_timestamp =
		shared_globalStats->stat_reset_timestamp;

	/*
	 * Try to open the stats file. If it doesn't exist, the backends simply
	 * return zero for anything and the collector simply starts from scratch
	 * with empty counters.
	 *
	 * ENOENT is a possibility if the stats collector is not running or has
	 * not yet written the stats file the first time.  Any other failure
	 * condition is suspicious.
	 */
	if ((fpin = AllocateFile(statfile, PG_BINARY_R)) == NULL)
	{
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open statistics file \"%s\": %m",
							statfile)));
		return;
	}

	/*
	 * Verify it's of the expected format.
	 */
	if (fread(&format_id, 1, sizeof(format_id), fpin) != sizeof(format_id) ||
		format_id != PGSTAT_FILE_FORMAT_ID)
	{
		ereport(LOG,
				(errmsg("corrupted statistics file \"%s\"", statfile)));
		goto done;
	}

	/*
	 * Read global stats struct
	 */
	if (fread(shared_globalStats, 1, sizeof(*shared_globalStats), fpin) !=
		sizeof(*shared_globalStats))
	{
		ereport(LOG,
				(errmsg("corrupted statistics file \"%s\"", statfile)));
		MemSet(shared_globalStats, 0, sizeof(*shared_globalStats));
		goto done;
	}

	/*
	 * In the collector, disregard the timestamp we read from the permanent
	 * stats file; we should be willing to write a temp stats file immediately
	 * upon the first request from any backend.  This only matters if the old
	 * file's timestamp is less than PGSTAT_STAT_INTERVAL ago, but that's not
	 * an unusual scenario.
	 */
	shared_globalStats->stats_timestamp = 0;

	/*
	 * Read archiver stats struct
	 */
	if (fread(shared_archiverStats, 1, sizeof(*shared_archiverStats), fpin) !=
		sizeof(*shared_archiverStats))
	{
		ereport(LOG,
				(errmsg("corrupted statistics file \"%s\"", statfile)));
		MemSet(shared_archiverStats, 0, sizeof(*shared_archiverStats));
		goto done;
	}

	/*
	 * We found an existing collector stats file. Read it and put all the
	 * hashtable entries into place.
	 */
	for (;;)
	{
		switch (fgetc(fpin))
		{
				/*
				 * 'D'	A PgStat_StatDBEntry struct describing a database
				 * follows.
				 */
			case 'D':
				if (fread(&dbbuf, 1, offsetof(PgStat_StatDBEntry, generation),
						  fpin) != offsetof(PgStat_StatDBEntry, generation))
				{
					ereport(LOG,
							(errmsg("corrupted statistics file \"%s\"",
									statfile)));
					goto done;
				}

				/*
				 * Add to the DB hash
				 */
				dbentry = (PgStat_StatDBEntry *)
					dshash_find_or_insert(pgStatDBHash, (void *) &dbbuf.databaseid,
										  &found);

				/* don't allow duplicate dbentries */
				if (found)
				{
					dshash_release_lock(pgStatDBHash, dbentry);
					ereport(LOG,
							(errmsg("corrupted statistics file \"%s\"",
									statfile)));
					goto done;
				}

				/* initialize the new shared entry */
				init_dbentry(dbentry);

				memcpy(dbentry, &dbbuf,
					   offsetof(PgStat_StatDBEntry, generation));

				/* Read the data from the database-specific file. */
				pgstat_read_pgStatDBHashfile(dbentry);
				dshash_release_lock(pgStatDBHash, dbentry);
				break;

			case 'E':
				goto done;

			default:
				ereport(LOG,
						(errmsg("corrupted statistics file \"%s\"",
								statfile)));
				goto done;
		}
	}

done:
	FreeFile(fpin);

	elog(DEBUG2, "removing permanent stats file \"%s\"", statfile);
	unlink(statfile);

	return;
}


/* ----------
 * pgstat_read_pgStatDBHashfile() -
 *
 *	Reads in the at-rest statistics file and create shared statistics
 *	tables. The file is removed after reading.
 * ----------
 */
static void
pgstat_read_pgStatDBHashfile(PgStat_StatDBEntry *dbentry)
{
	PgStat_StatTabEntry   *tabentry;
	PgStat_StatTabEntry		tabbuf;
	PgStat_StatFuncEntry	funcbuf;
	PgStat_StatFuncEntry   *funcentry;
	dshash_table		   *tabhash = NULL;
	dshash_table		   *funchash = NULL;
	FILE	   *fpin;
	int32		format_id;
	bool		found;
	char		statfile[MAXPGPATH];

	/* should be called from postmaster  */
	Assert(!IsUnderPostmaster);

	get_dbstat_filename(false, dbentry->databaseid, statfile, MAXPGPATH);

	/*
	 * Try to open the stats file. If it doesn't exist, the backends simply
	 * return zero for anything and the collector simply starts from scratch
	 * with empty counters.
	 *
	 * ENOENT is a possibility if the stats collector is not running or has
	 * not yet written the stats file the first time.  Any other failure
	 * condition is suspicious.
	 */
	if ((fpin = AllocateFile(statfile, PG_BINARY_R)) == NULL)
	{
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open statistics file \"%s\": %m",
							statfile)));
		return;
	}

	/*
	 * Verify it's of the expected format.
	 */
	if (fread(&format_id, 1, sizeof(format_id), fpin) != sizeof(format_id) ||
		format_id != PGSTAT_FILE_FORMAT_ID)
	{
		ereport(LOG,
				(errmsg("corrupted statistics file \"%s\"", statfile)));
		goto done;
	}

	/*
	 * We found an existing statistics file. Read it and put all the hashtable
	 * entries into place.
	 */
	for (;;)
	{
		switch (fgetc(fpin))
		{
				/*
				 * 'T'	A PgStat_StatTabEntry follows.
				 */
			case 'T':
				if (fread(&tabbuf, 1, sizeof(PgStat_StatTabEntry),
						  fpin) != sizeof(PgStat_StatTabEntry))
				{
					ereport(LOG,
							(errmsg("corrupted statistics file \"%s\"",
									statfile)));
					goto done;
				}

				if (tabhash == NULL)
				{
					tabhash = dshash_create(area, &dsh_tblparams, 0);
					dbentry->tables =
						dshash_get_hash_table_handle(tabhash);
				}

				tabentry = (PgStat_StatTabEntry *)
					dshash_find_or_insert(tabhash,
										  (void *) &tabbuf.tableid, &found);

				/* don't allow duplicate entries */
				if (found)
				{
					dshash_release_lock(tabhash, tabentry);
					ereport(LOG,
							(errmsg("corrupted statistics file \"%s\"",
									statfile)));
					goto done;
				}

				memcpy(tabentry, &tabbuf, sizeof(tabbuf));
				dshash_release_lock(tabhash, tabentry);
				break;

				/*
				 * 'F'	A PgStat_StatFuncEntry follows.
				 */
			case 'F':
				if (fread(&funcbuf, 1, sizeof(PgStat_StatFuncEntry),
						  fpin) != sizeof(PgStat_StatFuncEntry))
				{
					ereport(LOG,
							(errmsg("corrupted statistics file \"%s\"",
									statfile)));
					goto done;
				}

				if (funchash == NULL)
				{
					funchash = dshash_create(area, &dsh_tblparams, 0);
					dbentry->functions =
						dshash_get_hash_table_handle(funchash);
				}

				funcentry = (PgStat_StatFuncEntry *)
					dshash_find_or_insert(funchash,
										  (void *) &funcbuf.functionid, &found);

				if (found)
				{
					dshash_release_lock(funchash, funcentry);
					ereport(LOG,
							(errmsg("corrupted statistics file \"%s\"",
									statfile)));
					goto done;
				}

				memcpy(funcentry, &funcbuf, sizeof(funcbuf));
				dshash_release_lock(funchash, funcentry);
				break;

				/*
				 * 'E'	The EOF marker of a complete stats file.
				 */
			case 'E':
				goto done;

			default:
				ereport(LOG,
						(errmsg("corrupted statistics file \"%s\"",
								statfile)));
				goto done;
		}
	}

done:
	if (tabhash)
		dshash_detach(tabhash);
	if (funchash)
		dshash_detach(funchash);

	FreeFile(fpin);

	elog(DEBUG2, "removing permanent stats file \"%s\"", statfile);
	unlink(statfile);
}

/* ----------
 * pgstat_setup_memcxt() -
 *
 *	Create pgSharedStatsContext, if not already done.
 * ----------
 */
static void
pgstat_setup_memcxt(void)
{
	if (!pgStatLocalContext)
		pgStatLocalContext =
			AllocSetContextCreate(TopMemoryContext,
								  "Backend statistics snapshot",
								  ALLOCSET_SMALL_SIZES);
	if (!pgSharedStatsContext)
		pgSharedStatsContext =
			AllocSetContextCreate(TopMemoryContext,
								  "Shared activity statistics",
								  ALLOCSET_SMALL_SIZES);
}

/* ----------
 * pgstat_clear_snapshot() -
 *
 *	Discard any data collected in the current transaction.  Any subsequent
 *	request will cause new snapshots to be read.
 *
 *	This is also invoked during transaction commit or abort to discard
 *	the no-longer-wanted snapshot.
 * ----------
 */
void
pgstat_clear_snapshot(void)
{
	/* Release memory, if any was allocated */
	if (pgStatLocalContext)
	{
		MemoryContextDelete(pgStatLocalContext);

		/* Reset variables */
		pgStatLocalContext = NULL;
		localBackendStatusTable = NULL;
		localNumBackends = 0;
	}

	if (pgStatSnapshotContext)
	{
		MemoryContextReset(pgStatSnapshotContext);

		/* mark as the resource are not allocated */
		snapshot_globalStats = NULL;
		snapshot_archiverStats = NULL;
		pgStatDBEntrySnapshot = NULL;
	}
}

static bool
pgstat_update_tabentry(dshash_table *tabhash, PgStat_TableStatus *stat,
					   bool nowait)
{
	PgStat_StatTabEntry *tabentry;
	bool	found;

	if (tabhash == NULL)
		return false;

	tabentry = (PgStat_StatTabEntry *)
		dshash_find_or_insert_extended(tabhash, (void *) &(stat->t_id),
									   &found, nowait);

	/* failed to acquire lock */
	if (tabentry == NULL)
		return false;

	if (!found)
	{
		/*
		 * If it's a new table entry, initialize counters to the values we
		 * just got.
		 */
		tabentry->numscans = stat->t_counts.t_numscans;
		tabentry->tuples_returned = stat->t_counts.t_tuples_returned;
		tabentry->tuples_fetched = stat->t_counts.t_tuples_fetched;
		tabentry->tuples_inserted = stat->t_counts.t_tuples_inserted;
		tabentry->tuples_updated = stat->t_counts.t_tuples_updated;
		tabentry->tuples_deleted = stat->t_counts.t_tuples_deleted;
		tabentry->tuples_hot_updated = stat->t_counts.t_tuples_hot_updated;
		tabentry->n_live_tuples = stat->t_counts.t_delta_live_tuples;
		tabentry->n_dead_tuples = stat->t_counts.t_delta_dead_tuples;
		tabentry->changes_since_analyze = stat->t_counts.t_changed_tuples;
		tabentry->blocks_fetched = stat->t_counts.t_blocks_fetched;
		tabentry->blocks_hit = stat->t_counts.t_blocks_hit;

		tabentry->vacuum_timestamp = 0;
		tabentry->vacuum_count = 0;
		tabentry->autovac_vacuum_timestamp = 0;
		tabentry->autovac_vacuum_count = 0;
		tabentry->analyze_timestamp = 0;
		tabentry->analyze_count = 0;
		tabentry->autovac_analyze_timestamp = 0;
		tabentry->autovac_analyze_count = 0;
	}
	else
	{
		/*
		 * Otherwise add the values to the existing entry.
		 */
		tabentry->numscans += stat->t_counts.t_numscans;
		tabentry->tuples_returned += stat->t_counts.t_tuples_returned;
		tabentry->tuples_fetched += stat->t_counts.t_tuples_fetched;
		tabentry->tuples_inserted += stat->t_counts.t_tuples_inserted;
		tabentry->tuples_updated += stat->t_counts.t_tuples_updated;
		tabentry->tuples_deleted += stat->t_counts.t_tuples_deleted;
		tabentry->tuples_hot_updated += stat->t_counts.t_tuples_hot_updated;
		/* If table was truncated, first reset the live/dead counters */
		if (stat->t_counts.t_truncated)
		{
			tabentry->n_live_tuples = 0;
			tabentry->n_dead_tuples = 0;
		}
		tabentry->n_live_tuples += stat->t_counts.t_delta_live_tuples;
		tabentry->n_dead_tuples += stat->t_counts.t_delta_dead_tuples;
		tabentry->changes_since_analyze += stat->t_counts.t_changed_tuples;
		tabentry->blocks_fetched += stat->t_counts.t_blocks_fetched;
		tabentry->blocks_hit += stat->t_counts.t_blocks_hit;
	}

	/* Clamp n_live_tuples in case of negative delta_live_tuples */
	tabentry->n_live_tuples = Max(tabentry->n_live_tuples, 0);
	/* Likewise for n_dead_tuples */
	tabentry->n_dead_tuples = Max(tabentry->n_dead_tuples, 0);

	dshash_release_lock(tabhash, tabentry);

	return true;
}

static void
pgstat_update_dbentry(PgStat_StatDBEntry *dbentry, PgStat_TableStatus *stat)
{
	/*
	 * Add per-table stats to the per-database entry, too.
	 */
	LWLockAcquire(&dbentry->lock, LW_EXCLUSIVE);
	dbentry->n_tuples_returned += stat->t_counts.t_tuples_returned;
	dbentry->n_tuples_fetched += stat->t_counts.t_tuples_fetched;
	dbentry->n_tuples_inserted += stat->t_counts.t_tuples_inserted;
	dbentry->n_tuples_updated += stat->t_counts.t_tuples_updated;
	dbentry->n_tuples_deleted += stat->t_counts.t_tuples_deleted;
	dbentry->n_blocks_fetched += stat->t_counts.t_blocks_fetched;
	dbentry->n_blocks_hit += stat->t_counts.t_blocks_hit;
	LWLockRelease(&dbentry->lock);
}

/*
 * Lookup the hash table entry for the specified database. If no hash
 * table entry exists, initialize it, if the create parameter is true.
 * Else, return NULL.
 */
static PgStat_StatDBEntry *
pgstat_get_db_entry(Oid databaseid, int op,	PgStat_TableLookupState *status)
{
	PgStat_StatDBEntry *result;
	bool		nowait = ((op & PGSTAT_FETCH_NOWAIT) != 0);
	bool		lock_acquired = true;
	bool		found = true;

	if (!IsUnderPostmaster)
		return NULL;

	Assert(pgStatDBHash);

	/* Lookup or create the hash table entry for this database */
	if (op & PGSTAT_FETCH_EXCLUSIVE)
	{
		result = (PgStat_StatDBEntry *)
			dshash_find_or_insert_extended(pgStatDBHash, &databaseid,
										   &found, nowait);
		if (result == NULL)
			lock_acquired = false;
		else if (!found)
		{
			/*
			 * If not found, initialize the new one.  This creates empty hash
			 * tables hash, too.
			 */
			init_dbentry(result);
			reset_dbentry_counters(result);
		}
	}
	else
	{
		result = (PgStat_StatDBEntry *)
			dshash_find_extended(pgStatDBHash, &databaseid, true, nowait,
								 nowait ? &lock_acquired : NULL);
		if (result == NULL)
			found = false;
	}

	/* Set return status if requested */
	if (status)
	{
		if (!lock_acquired)
		{
			Assert(nowait);
			*status = PGSTAT_ENTRY_LOCK_FAILED;
		}
		else if (!found)
			*status = PGSTAT_ENTRY_NOT_FOUND;
		else
			*status = PGSTAT_ENTRY_FOUND;
	}

	return result;
}

/*
 * Lookup the hash table entry for the specified table. If no hash
 * table entry exists, initialize it, if the create parameter is true.
 * Else, return NULL.
 */
static PgStat_StatTabEntry *
pgstat_get_tab_entry(dshash_table *table, Oid tableoid, bool create)
{
	PgStat_StatTabEntry *result;
	bool		found;

	/* Lookup or create the hash table entry for this table */
	if (create)
		result = (PgStat_StatTabEntry *)
			dshash_find_or_insert(table, &tableoid, &found);
	else
		result = (PgStat_StatTabEntry *) dshash_find(table, &tableoid, false);

	if (!create && !found)
		return NULL;

	/* If not found, initialize the new one. */
	if (!found)
	{
		result->numscans = 0;
		result->tuples_returned = 0;
		result->tuples_fetched = 0;
		result->tuples_inserted = 0;
		result->tuples_updated = 0;
		result->tuples_deleted = 0;
		result->tuples_hot_updated = 0;
		result->n_live_tuples = 0;
		result->n_dead_tuples = 0;
		result->changes_since_analyze = 0;
		result->blocks_fetched = 0;
		result->blocks_hit = 0;
		result->vacuum_timestamp = 0;
		result->vacuum_count = 0;
		result->autovac_vacuum_timestamp = 0;
		result->autovac_vacuum_count = 0;
		result->analyze_timestamp = 0;
		result->analyze_count = 0;
		result->autovac_analyze_timestamp = 0;
		result->autovac_analyze_count = 0;
	}

	return result;
}

/*
 * Convert a potentially unsafely truncated activity string (see
 * PgBackendStatus.st_activity_raw's documentation) into a correctly truncated
 * one.
 *
 * The returned string is allocated in the caller's memory context and may be
 * freed.
 */
char *
pgstat_clip_activity(const char *raw_activity)
{
	char	   *activity;
	int			rawlen;
	int			cliplen;

	/*
	 * Some callers, like pgstat_get_backend_current_activity(), do not
	 * guarantee that the buffer isn't concurrently modified. We try to take
	 * care that the buffer is always terminated by a NUL byte regardless, but
	 * let's still be paranoid about the string's length. In those cases the
	 * underlying buffer is guaranteed to be pgstat_track_activity_query_size
	 * large.
	 */
	activity = pnstrdup(raw_activity, pgstat_track_activity_query_size - 1);

	/* now double-guaranteed to be NUL terminated */
	rawlen = strlen(activity);

	/*
	 * All supported server-encodings make it possible to determine the length
	 * of a multi-byte character from its first byte (this is not the case for
	 * client encodings, see GB18030). As st_activity is always stored using
	 * server encoding, this allows us to perform multi-byte aware truncation,
	 * even if the string earlier was truncated in the middle of a multi-byte
	 * character.
	 */
	cliplen = pg_mbcliplen(activity, rawlen,
						   pgstat_track_activity_query_size - 1);

	activity[cliplen] = '\0';

	return activity;
}
