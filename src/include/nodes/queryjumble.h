/*-------------------------------------------------------------------------
 *
 * queryjumble.h
 *	  Query normalization and fingerprinting.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/nodes/queryjumble.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUERYJUMBLE_H
#define QUERYJUMBLE_H

#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"

/*
 * Struct for tracking locations/lengths of constants during normalization
 */
typedef struct LocationLen
{
	int			location;		/* start offset in query text */
	int			length;			/* length in bytes, or -1 to ignore */

	/*
	 * Indicates that this location represents the beginning or end of a run
	 * of squashed constants.
	 */
	bool		squashed;
} LocationLen;

/*
 * Working state for computing a query jumble and producing a normalized
 * query string
 */
typedef struct JumbleState
{
	/* Jumble of current query tree */
	unsigned char *jumble;

	/* Number of bytes used in jumble[] */
	Size		jumble_len;

	/* Array of locations of constants that should be removed */
	LocationLen *clocations;

	/* Allocated length of clocations array */
	int			clocations_buf_size;

	/* Current number of valid entries in clocations array */
	int			clocations_count;

	/* highest Param id we've seen, in order to start normalization correctly */
	int			highest_extern_param_id;

	/*
	 * Count of the number of NULL nodes seen since last appending a value.
	 * These are flushed out to the jumble buffer before subsequent appends
	 * and before performing the final jumble hash.
	 */
	unsigned int pending_nulls;

#ifdef USE_ASSERT_CHECKING
	/* The total number of bytes added to the jumble buffer */
	Size		total_jumble_len;
#endif
} JumbleState;

/* Values for the compute_query_id GUC */
enum ComputeQueryIdType
{
	COMPUTE_QUERY_ID_OFF,
	COMPUTE_QUERY_ID_ON,
	COMPUTE_QUERY_ID_AUTO,
	COMPUTE_QUERY_ID_REGRESS,
};

/* Values for the compute_plan_id GUC */
enum ComputePlanIdType
{
	COMPUTE_PLAN_ID_OFF,
	COMPUTE_PLAN_ID_ON,
	COMPUTE_PLAN_ID_AUTO,
	COMPUTE_PLAN_ID_REGRESS,
};

/* GUC parameters */
extern PGDLLIMPORT int compute_query_id;
extern PGDLLIMPORT int compute_plan_id;


/*
 * Generic routines for expression jumbling.
 *
 * XXX: It may be better to separate these routines in a separate
 * file.
 */
extern JumbleState *InitJumble(void);
extern void JumbleNode(JumbleState *jstate, Node *node);
extern uint64 HashJumbleState(JumbleState *jstate);

extern void AppendJumble(JumbleState *jstate,
						 const unsigned char *item, Size size);
extern void AppendJumble8(JumbleState *jstate, const unsigned char *value);
extern void AppendJumble16(JumbleState *jstate, const unsigned char *value);
extern void AppendJumble32(JumbleState *jstate, const unsigned char *value);
extern void AppendJumble64(JumbleState *jstate, const unsigned char *value);

/*
 * AppendJumbleNull
 *		For jumbling NULL pointers
 */
static pg_attribute_always_inline void
AppendJumbleNull(JumbleState *jstate)
{
	jstate->pending_nulls++;
}

#define JUMBLE_VALUE(val) \
do { \
	if (sizeof(val) == 8) \
		AppendJumble64(jstate, (const unsigned char *) &(val)); \
	else if (sizeof(val) == 4) \
		AppendJumble32(jstate, (const unsigned char *) &(val)); \
	else if (sizeof(val) == 2) \
		AppendJumble16(jstate, (const unsigned char *) &(val)); \
	else if (sizeof(val) == 1) \
		AppendJumble8(jstate, (const unsigned char *) &(val)); \
	else \
		AppendJumble(jstate, (const unsigned char *) &(val), sizeof(val)); \
} while (0)
#define JUMBLE_VALUE_STRING(str) \
do { \
	if (str) \
		AppendJumble(jstate, (const unsigned char *) (str), strlen(str) + 1); \
	else \
		AppendJumbleNull(jstate); \
} while(0)

/* Query jumbling routines */
extern const char *CleanQuerytext(const char *query, int *location, int *len);
extern JumbleState *JumbleQuery(Query *query);
extern void EnableQueryId(void);
extern void EnablePlanId(void);

extern PGDLLIMPORT bool query_id_enabled;
extern PGDLLIMPORT bool plan_id_enabled;

/* Plan jumbling routines */
extern void JumbleRangeTable(JumbleState *jstate, List *rtable);

/*
 * Returns whether query identifier computation has been enabled, either
 * directly in the GUC or by a module when the setting is 'auto'.
 */
static inline bool
IsQueryIdEnabled(void)
{
	if (compute_query_id == COMPUTE_QUERY_ID_OFF)
		return false;
	if (compute_query_id == COMPUTE_QUERY_ID_ON)
		return true;
	return query_id_enabled;
}

/*
 * Returns whether plan identifier computation has been enabled, either
 * directly in the GUC or by a module when the setting is 'auto'.
 */
static inline bool
IsPlanIdEnabled(void)
{
	if (compute_plan_id == COMPUTE_PLAN_ID_OFF)
		return false;
	if (compute_plan_id == COMPUTE_PLAN_ID_ON)
		return true;
	return plan_id_enabled;
}

#endif							/* QUERYJUMBLE_H */
