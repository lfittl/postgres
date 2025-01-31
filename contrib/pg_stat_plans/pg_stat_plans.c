/*--------------------------------------------------------------------------
 *
 * pg_stat_plans.c
 *		Track per-plan call counts, execution times and EXPLAIN texts
 *		across a whole database cluster.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		contrib/pg_stat_plans/pg_stat_plans.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/parallel.h"
#include "catalog/pg_authid.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "nodes/queryjumble.h"
#include "pgstat.h"
#include "optimizer/planner.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pgstat_internal.h"
#include "utils/snapmgr.h"

PG_MODULE_MAGIC;

/* Current nesting depth of planner/ExecutorRun/ProcessUtility calls */
static int	nesting_level = 0;

/* Saved hook values */
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/*---- GUC variables ----*/

typedef enum
{
	PGSP_TRACK_NONE,			/* track no plans */
	PGSP_TRACK_TOP,				/* only plans for top level statements */
	PGSP_TRACK_ALL,				/* all plans, including for nested statements */
}			PGSPTrackLevel;

static const struct config_enum_entry track_options[] =
{
	{"none", PGSP_TRACK_NONE, false},
	{"top", PGSP_TRACK_TOP, false},
	{"all", PGSP_TRACK_ALL, false},
	{NULL, 0, false}
};

static int	pgsp_max = 5000;	/* max # plans to track */
static int	pgsp_max_size = 2048;	/* max size of plan text to track (in
									 * bytes) */
static int	pgsp_track = PGSP_TRACK_TOP;	/* tracking level */

#define pgsp_enabled(level) \
	(!IsParallelWorker() && \
	(compute_plan_id != COMPUTE_PLAN_ID_OFF) && \
	(pgsp_track == PGSP_TRACK_ALL || \
	(pgsp_track == PGSP_TRACK_TOP && (level) == 0)))

#define USAGE_INCREASE			0.5 /* increase by this each time we report
									 * stats */
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every
										 * pgstat_dealloc_plans */
#define USAGE_DEALLOC_PERCENT	5	/* free this % of entries at once */

/*---- Function declarations ----*/

PG_FUNCTION_INFO_V1(pg_stat_plans_reset);
PG_FUNCTION_INFO_V1(pg_stat_plans_1_0);

/* Structures for statistics of plans */
typedef struct PgStatShared_PlanInfo
{
	/* key elements that identify a plan (together with the dboid) */
	uint64		planid;
	uint64		queryid;
	Oid			userid;			/* userid is tracked to allow users to see
								 * their own query plans */
	bool		toplevel;		/* query executed at top level */

	dsa_pointer plan_text;		/* pointer to DSA memory containing plan text */
	int			plan_encoding;	/* plan text encoding */
}			PgStatShared_PlanInfo;

typedef struct PgStat_StatPlanEntry
{
	PgStat_Counter exec_count;
	double		exec_time;
	double		usage;			/* Usage factor of the entry, used to
								 * prioritize which plans to age out */

	/* Only used in shared structure, not in local pending stats */
	PgStatShared_PlanInfo info;
}			PgStat_StatPlanEntry;

typedef struct PgStatShared_Plan
{
	PgStatShared_Common header;
	PgStat_StatPlanEntry stats;
}			PgStatShared_Plan;

static bool plan_stats_flush_cb(PgStat_EntryRef *entry_ref, bool nowait);

static const PgStat_KindInfo plan_stats = {
	.name = "plan_stats",
	.fixed_amount = false,

	/*
	 * We currently don't write to a file since plan texts would get lost (and
	 * just the stats on their own aren't that useful)
	 */
	.write_to_file = false,

	/*
	 * Plan statistics are available system-wide to simplify monitoring
	 * scripts
	 */
	.accessed_across_databases = true,

	.shared_size = sizeof(PgStatShared_Plan),
	.shared_data_off = offsetof(PgStatShared_Plan, stats),
	.shared_data_len = sizeof(((PgStatShared_Plan *) 0)->stats),
	.pending_size = sizeof(PgStat_StatPlanEntry),
	.flush_pending_cb = plan_stats_flush_cb,
};

/*
 * Compute stats entry idx from query ID and plan ID with an 8-byte hash.
 *
 * Whilst we could theorically just use the plan ID here, we intentionally
 * add the query ID into the mix to ease interpreting the data in combination
 * with pg_stat_statements.
 */
#define PGSTAT_PLAN_IDX(query_id, plan_id, user_id, toplevel) hash_combine64(toplevel, hash_combine64(query_id, hash_combine64(plan_id, user_id)))

/*
 * Kind ID reserved for statistics of plans.
 */
#define PGSTAT_KIND_PLANS	PGSTAT_KIND_EXPERIMENTAL	/* TODO: Assign */

/*
 * Callback for stats handling
 */
static bool
plan_stats_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStat_StatPlanEntry *localent;
	PgStatShared_Plan *shfuncent;

	localent = (PgStat_StatPlanEntry *) entry_ref->pending;
	shfuncent = (PgStatShared_Plan *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	shfuncent->stats.exec_count += localent->exec_count;
	shfuncent->stats.exec_time += localent->exec_time;
	shfuncent->stats.usage += localent->usage;

	pgstat_unlock_entry(entry_ref);

	return true;
}

static char *
pgsp_explain_plan(QueryDesc *queryDesc)
{
	ExplainState *es;
	StringInfo	es_str;

	es = NewExplainState();
	es_str = es->str;

	/*
	 * We turn off COSTS since identical planids may have very different
	 * costs, and it could be misleading to only show the first recorded
	 * plan's costs.
	 */
	es->costs = false;
	es->format = EXPLAIN_FORMAT_TEXT;

	ExplainBeginOutput(es);
	ExplainPrintPlan(es, queryDesc);
	ExplainEndOutput(es);

	return es_str->data;
}

static void
pgstat_gc_plan_memory()
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *p;

	/* dshash entry is not modified, take shared lock */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, false);
	while ((p = dshash_seq_next(&hstat)) != NULL)
	{
		PgStatShared_Common *header;
		PgStat_StatPlanEntry *statent;

		if (!p->dropped || p->key.kind != PGSTAT_KIND_PLANS)
			continue;

		header = dsa_get_address(pgStatLocal.dsa, p->body);

		if (!LWLockConditionalAcquire(&header->lock, LW_EXCLUSIVE))
			continue;

		statent = (PgStat_StatPlanEntry *) pgstat_get_entry_data(PGSTAT_KIND_PLANS, header);

		/*
		 * Clean up this entry's plan text allocation, if we haven't done so
		 * already
		 */
		if (DsaPointerIsValid(statent->info.plan_text))
		{
			dsa_free(pgStatLocal.dsa, statent->info.plan_text);
			statent->info.plan_text = InvalidDsaPointer;

			/* Allow removal of the shared stats entry */
			pg_atomic_fetch_sub_u32(&p->refcount, 1);
		}

		LWLockRelease(&header->lock);
	}
	dshash_seq_term(&hstat);

	/* Encourage other backends to clean up dropped entry refs */
	pgstat_request_entry_refs_gc();
}

typedef struct PlanDeallocEntry
{
	PgStat_HashKey key;
	double		usage;
}			PlanDeallocEntry;

/*
 * list sort comparator for sorting into decreasing usage order
 */
static int
entry_cmp_lru(const union ListCell *lhs, const union ListCell *rhs)
{
	double		l_usage = ((PlanDeallocEntry *) lfirst(lhs))->usage;
	double		r_usage = ((PlanDeallocEntry *) lfirst(rhs))->usage;

	if (l_usage > r_usage)
		return -1;
	else if (l_usage < r_usage)
		return +1;
	else
		return 0;
}

static void
pgstat_dealloc_plans()
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *p;
	List	   *entries = NIL;
	ListCell   *lc;
	int			nvictims;

	/* dshash entry is not modified, take shared lock */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, false);
	while ((p = dshash_seq_next(&hstat)) != NULL)
	{
		PgStatShared_Common *header;
		PgStat_StatPlanEntry *statent;
		PlanDeallocEntry *entry;

		if (p->dropped || p->key.kind != PGSTAT_KIND_PLANS)
			continue;

		header = dsa_get_address(pgStatLocal.dsa, p->body);

		if (!LWLockConditionalAcquire(&header->lock, LW_EXCLUSIVE))
			continue;

		statent = (PgStat_StatPlanEntry *) pgstat_get_entry_data(PGSTAT_KIND_PLANS, header);
		statent->usage *= USAGE_DECREASE_FACTOR;

		entry = palloc(sizeof(PlanDeallocEntry));
		entry->key = p->key;
		entry->usage = statent->usage;

		LWLockRelease(&header->lock);

		entries = lappend(entries, entry);
	}
	dshash_seq_term(&hstat);

	/* Sort by usage ascending (lowest used entries are last) */
	list_sort(entries, entry_cmp_lru);

	/* At a minimum, deallocate 10 entries to make it worth our while */
	nvictims = Max(10, list_length(entries) * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, list_length(entries));

	/* Actually drop the entries */
	for_each_from(lc, entries, list_length(entries) - nvictims)
	{
		PlanDeallocEntry *entry = lfirst(lc);

		pgstat_drop_entry(entry->key.kind, entry->key.dboid, entry->key.objid);
	}

	/* Clean up our working memory immediately */
	foreach(lc, entries)
	{
		PlanDeallocEntry *entry = lfirst(lc);

		pfree(entry);
	}
	pfree(entries);
}

static void
pgstat_gc_plans()
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *p;
	bool		have_dropped_entries = false;
	size_t		plan_entry_count = 0;

	/* TODO: Prevent concurrent GC cycles - flag an active GC run somehow */

	/*
	 * Count our active entries, and whether there are any dropped entries we
	 * may need to clean up at the end.
	 */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, false);
	while ((p = dshash_seq_next(&hstat)) != NULL)
	{
		if (p->key.kind != PGSTAT_KIND_PLANS)
			continue;

		if (p->dropped)
			have_dropped_entries = true;
		else
			plan_entry_count++;
	}
	dshash_seq_term(&hstat);

	/*
	 * If we're over the limit, delete entries with lowest usage factor.
	 */
	if (plan_entry_count > pgsp_max)
	{
		pgstat_dealloc_plans();
		have_dropped_entries = true;	/* Assume we did some work */
	}

	/* If there are dropped entries, clean up their plan memory if needed */
	if (have_dropped_entries)
		pgstat_gc_plan_memory();
}

static void
pgstat_report_plan_stats(QueryDesc *queryDesc,
						 PgStat_Counter exec_count,
						 double exec_time)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Plan *shstatent;
	PgStat_StatPlanEntry *statent;
	bool		newly_created;
	uint64		queryId = queryDesc->plannedstmt->queryId;
	uint64		planId = queryDesc->plannedstmt->planId;
	Oid			userid = GetUserId();
	bool		toplevel = (nesting_level == 0);

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_PLANS, MyDatabaseId,
										  PGSTAT_PLAN_IDX(queryId, planId, userid, toplevel), &newly_created);

	shstatent = (PgStatShared_Plan *) entry_ref->shared_stats;
	statent = &shstatent->stats;

	if (newly_created)
	{
		char	   *plan = pgsp_explain_plan(queryDesc);
		size_t		plan_size = Min(strlen(plan), pgsp_max_size);

		(void) pgstat_lock_entry(entry_ref, false);

		/*
		 * We may be over the limit, so run GC now before saving entry (we do
		 * this whilst holding the lock on the new entry so we don't remove it
		 * by accident)
		 */
		pgstat_gc_plans();

		shstatent->stats.info.planid = planId;
		shstatent->stats.info.queryid = queryId;
		shstatent->stats.info.userid = userid;
		shstatent->stats.info.toplevel = toplevel;
		shstatent->stats.info.plan_text = dsa_allocate(pgStatLocal.dsa, plan_size);
		strlcpy(dsa_get_address(pgStatLocal.dsa, shstatent->stats.info.plan_text), plan, plan_size);

		shstatent->stats.info.plan_encoding = GetDatabaseEncoding();

		/*
		 * Increase refcount here so entry can't get released without us
		 * dropping the plan text
		 */
		pg_atomic_fetch_add_u32(&entry_ref->shared_entry->refcount, 1);

		pgstat_unlock_entry(entry_ref);

		pfree(plan);
	}

	statent->exec_count += exec_count;
	statent->exec_time += exec_time;
	statent->usage += USAGE_INCREASE;
}

/*
 * Planner hook: forward to regular planner, but increase plan count and
 * record query plan if needed.
 */
static PlannedStmt *
pgsp_planner(Query *parse,
			 const char *query_string,
			 int cursorOptions,
			 ParamListInfo boundParams)
{
	PlannedStmt *result;

	/*
	 * Increment the nesting level, to ensure that functions evaluated during
	 * planning are not seen as top-level calls.
	 */
	nesting_level++;
	PG_TRY();
	{
		if (prev_planner_hook)
			result = prev_planner_hook(parse, query_string, cursorOptions,
									   boundParams);
		else
			result = standard_planner(parse, query_string, cursorOptions,
									  boundParams);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();

	return result;
}

/*
 * ExecutorStart hook: start up tracking if needed
 */
static bool
pgsp_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	bool		plan_valid;
	uint64		queryId = queryDesc->plannedstmt->queryId;
	uint64		planId = queryDesc->plannedstmt->planId;

	if (prev_ExecutorStart)
		plan_valid = prev_ExecutorStart(queryDesc, eflags);
	else
		plan_valid = standard_ExecutorStart(queryDesc, eflags);

	/* The plan may have become invalid during standard_ExecutorStart() */
	if (!plan_valid)
		return false;

	if (queryId != UINT64CONST(0) && planId != UINT64CONST(0) &&
		pgsp_enabled(nesting_level))
	{
		/*
		 * Record initial entry now, so plan text is available for currently
		 * running queries
		 */
		pgstat_report_plan_stats(queryDesc,
								 0, /* executions are counted in
									 * pgsp_ExecutorEnd */
								 0.0);

		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
			MemoryContextSwitchTo(oldcxt);
		}
	}

	return true;
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgsp_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pgsp_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: store results if needed
 */
static void
pgsp_ExecutorEnd(QueryDesc *queryDesc)
{
	uint64		queryId = queryDesc->plannedstmt->queryId;
	uint64		planId = queryDesc->plannedstmt->planId;

	if (queryId != UINT64CONST(0) && planId != UINT64CONST(0) &&
		queryDesc->totaltime && pgsp_enabled(nesting_level))
	{
		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if several
		 * levels of hook all do this.)
		 */
		InstrEndLoop(queryDesc->totaltime);

		pgstat_report_plan_stats(queryDesc,
								 1,
								 queryDesc->totaltime->total * 1000.0 /* convert to msec */ );
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to register for shared memory stats, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_plans functions to be created even when the module
	 * isn't active.  The functions must protect themselves against being
	 * called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Inform the postmaster that we want to enable query_id calculation if
	 * compute_query_id is set to auto, as well as plan_id calculation if
	 * compute_plan_id is set to auto.
	 */
	EnableQueryId();
	EnablePlanId();

	/*
	 * Define (or redefine) custom GUC variables.
	 */
	DefineCustomIntVariable("pg_stat_plans.max",
							"Sets the maximum number of plans tracked by pg_stat_plans in shared memory.",
							NULL,
							&pgsp_max,
							5000,
							100,
							INT_MAX / 2,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_stat_plans.max_size",
							"Sets the maximum size of plan texts tracked by pg_stat_plans in shared memory.",
							NULL,
							&pgsp_max_size,
							2048,
							100,
							1048576,	/* 1MB hard limit */
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("pg_stat_plans.track",
							 "Selects which plans are tracked by pg_stat_plans.",
							 NULL,
							 &pgsp_track,
							 PGSP_TRACK_TOP,
							 track_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_stat_plans");

	/*
	 * Install hooks.
	 */
	prev_planner_hook = planner_hook;
	planner_hook = pgsp_planner;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgsp_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pgsp_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pgsp_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgsp_ExecutorEnd;

	pgstat_register_kind(PGSTAT_KIND_PLANS, &plan_stats);
}

static bool
match_plans_entries(PgStatShared_HashEntry *entry, Datum match_data)
{
	return entry->key.kind == PGSTAT_KIND_PLANS;
}

/*
 * Reset statement statistics.
 */
Datum
pg_stat_plans_reset(PG_FUNCTION_ARGS)
{
	pgstat_drop_matching_entries(match_plans_entries, 0);

	/* Free plan text memory and allow cleanup of dropped entries */
	pgstat_gc_plan_memory();

	PG_RETURN_VOID();
}

#define PG_STAT_PLANS_COLS 8

Datum
pg_stat_plans_1_0(PG_FUNCTION_ARGS)
{
	bool		showplan = PG_GETARG_BOOL(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			userid = GetUserId();
	bool		is_allowed_role = false;

	dshash_seq_status hstat;
	PgStatShared_HashEntry *p;

	/*
	 * Superusers or roles with the privileges of pg_read_all_stats members
	 * are allowed
	 */
	is_allowed_role = has_privs_of_role(userid, ROLE_PG_READ_ALL_STATS);

	/* stats kind must be registered already */
	if (!pgstat_get_kind_info(PGSTAT_KIND_PLANS))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_plans must be loaded via \"shared_preload_libraries\"")));

	InitMaterializedSRF(fcinfo, 0);

	/* dshash entry is not modified, take shared lock */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, false);
	while ((p = dshash_seq_next(&hstat)) != NULL)
	{
		PgStat_StatPlanEntry *statent;
		Datum		values[PG_STAT_PLANS_COLS];
		bool		nulls[PG_STAT_PLANS_COLS];
		int			i = 0;

		if (p->dropped || p->key.kind != PGSTAT_KIND_PLANS)
			continue;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		statent = pgstat_get_entry_data(p->key.kind, dsa_get_address(pgStatLocal.dsa, p->body));

		values[i++] = ObjectIdGetDatum(statent->info.userid);
		values[i++] = ObjectIdGetDatum(p->key.dboid);
		values[i++] = BoolGetDatum(statent->info.toplevel);
		if (is_allowed_role || statent->info.userid == userid)
		{
			int64		queryid = statent->info.queryid;
			int64		planid = statent->info.planid;

			values[i++] = Int64GetDatumFast(queryid);
			values[i++] = Int64GetDatumFast(planid);
		}
		else
		{
			nulls[i++] = true;
			nulls[i++] = true;
		}
		values[i++] = Int64GetDatumFast(statent->exec_count);
		values[i++] = Float8GetDatumFast(statent->exec_time);

		if (showplan && (is_allowed_role || statent->info.userid == userid))
		{
			char	   *pstr = DsaPointerIsValid(statent->info.plan_text) ? dsa_get_address(pgStatLocal.dsa, statent->info.plan_text) : NULL;

			if (pstr)
			{
				char	   *enc = pg_any_to_server(pstr, strlen(pstr), statent->info.plan_encoding);

				values[i++] = CStringGetTextDatum(enc);

				if (enc != pstr)
					pfree(enc);
			}
			else
			{
				nulls[i++] = true;
			}
		}
		else if (showplan)
		{
			values[i++] = CStringGetTextDatum("<insufficient privilege>");
		}
		else
		{
			nulls[i++] = true;
		}
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	dshash_seq_term(&hstat);

	return (Datum) 0;
}
