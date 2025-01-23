/*-------------------------------------------------------------------------
 *
 * planjumble.h
 *	  Plan fingerprinting.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/nodes/planjumble.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANJUMBLE_H
#define PLANJUMBLE_H

#include "nodes/plannodes.h"
#include "nodes/queryjumble.h"

/* Values for the compute_plan_id GUC */
enum ComputePlanIdType
{
	COMPUTE_PLAN_ID_OFF,
	COMPUTE_PLAN_ID_ON,
	COMPUTE_PLAN_ID_REGRESS,
};

/* GUC parameters */
extern PGDLLIMPORT int compute_plan_id;

extern void JumblePlanNode(JumbleState *jumble, List *rtable, Plan *plan);

#endif							/* PLANJUMBLE_H */
