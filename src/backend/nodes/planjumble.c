/*-------------------------------------------------------------------------
 *
 * planjumble.c
 *	 Plan fingerprinting.
 *
 * Calculates the plan fingerprint for a given plan tree. Note this works
 * in combination with the planner's setrefs functionality in order to
 * walk the tree.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/planjumble.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/planjumble.h"
#include "parser/parsetree.h"

#define JUMBLE_SIZE				1024	/* query serialization buffer size */

/* GUC parameters */
int			compute_plan_id = COMPUTE_PLAN_ID_OFF;

#define JUMBLE_VALUE(item) \
	AppendJumble(jstate, (const unsigned char *) &(item), sizeof(item))
#define JUMBLE_STRING(str) \
do { \
	if (str) \
		AppendJumble(jstate, (const unsigned char *) (str), strlen(str) + 1); \
} while(0)

/*
 * Jumble the target relation of a scan or modify node
 *
 * This functions similarly to ExplainTargetRel.
 */
static void
JumbleTargetRel(JumbleState *jstate, List *rtable, Plan *plan, Index rti)
{
	RangeTblEntry *rte = rt_fetch(rti, rtable);

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_ForeignScan:
		case T_CustomScan:
		case T_ModifyTable:
			/* Assert it's on a real relation */
			Assert(rte->rtekind == RTE_RELATION);
			JUMBLE_VALUE(rte->relid);
			break;
		case T_TableFuncScan:
			{
				TableFunc  *tablefunc = ((TableFuncScan *) plan)->tablefunc;

				Assert(rte->rtekind == RTE_TABLEFUNC);
				JUMBLE_VALUE(tablefunc->functype);
			}
			break;
		case T_ValuesScan:
			Assert(rte->rtekind == RTE_VALUES);
			break;
		case T_CteScan:
			/* Assert it's on a non-self-reference CTE */
			Assert(rte->rtekind == RTE_CTE);
			Assert(!rte->self_reference);
			JUMBLE_STRING(rte->ctename);
			break;
		case T_NamedTuplestoreScan:
			Assert(rte->rtekind == RTE_NAMEDTUPLESTORE);
			JUMBLE_STRING(rte->enrname);
			break;
		case T_WorkTableScan:
			/* Assert it's on a self-reference CTE */
			Assert(rte->rtekind == RTE_CTE);
			Assert(rte->self_reference);
			JUMBLE_STRING(rte->ctename);
			break;
		default:
			break;
	}
}

/*
 * Jumble the target of a Scan node
 */
static void
JumbleScanTarget(JumbleState *jstate, List *rtable, Scan *scan)
{
	JumbleTargetRel(jstate, rtable, (Plan *) scan, scan->scanrelid);
}

/*
 * JumblePlanNode: Append significant information to the plan identifier jumble
 *
 * Note this intentionally doesn't descend into child plan nodes, since the caller
 * already takes care of that.
 */
void
JumblePlanNode(JumbleState *jstate, List *rtable, Plan *plan)
{
	JUMBLE_VALUE(nodeTag(plan));
	JumbleNode(jstate, (Node *) plan->qual);
	JumbleNode(jstate, (Node *) plan->targetlist);

	/*
	 * Plan-type-specific fixes
	 */
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			{
				SeqScan    *splan = (SeqScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
			}
			break;
		case T_SampleScan:
			{
				SampleScan *splan = (SampleScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);

				/*
				 * TODO: It may be worth jumbling the properties of
				 * splan->tablesample
				 */
			}
			break;
		case T_IndexScan:
			{
				IndexScan  *splan = (IndexScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
				JUMBLE_VALUE(splan->indexid);
				JumbleNode(jstate, (Node *) splan->indexqual);
				/* Skip splan->indexqualorig */
				JumbleNode(jstate, (Node *) splan->indexorderby);
				/* Skip splan->indexorderbyorig */
				JUMBLE_VALUE(splan->indexorderdir);
			}
			break;
		case T_IndexOnlyScan:
			{
				IndexOnlyScan *splan = (IndexOnlyScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
				JUMBLE_VALUE(splan->indexid);
				JumbleNode(jstate, (Node *) splan->indexqual);
				/* Skip splan->recheckqual */
				JumbleNode(jstate, (Node *) splan->indexorderby);
				/* Skip splan->indextlist */
				JUMBLE_VALUE(splan->indexorderdir);
			}
			break;
		case T_BitmapIndexScan:
			{
				BitmapIndexScan *splan = (BitmapIndexScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
				JUMBLE_VALUE(splan->indexid);
				JumbleNode(jstate, (Node *) splan->indexqual);
				/* Skip splan->indexqualorig */
			}
			break;
		case T_BitmapHeapScan:
			{
				BitmapHeapScan *splan = (BitmapHeapScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
				/* Skip splan->bitmapqualorig */
			}
			break;
		case T_TidScan:
			{
				TidScan    *splan = (TidScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
				JumbleNode(jstate, (Node *) splan->tidquals);
			}
			break;
		case T_TidRangeScan:
			{
				TidRangeScan *splan = (TidRangeScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
				JumbleNode(jstate, (Node *) splan->tidrangequals);
			}
			break;
		case T_SubqueryScan:
			{
				SubqueryScan *splan = (SubqueryScan *) plan;

				/*
				 * TODO: JumbleScanHeader currently doesn't jumble the subplan
				 * name
				 */
				JumbleScanTarget(jstate, rtable, &splan->scan);
				/* We rely on the caller to descend into the actual subplans */
			}
			break;
		case T_FunctionScan:
			{
				FunctionScan *splan = (FunctionScan *) plan;

				/*
				 * If the expression is still a call of a single function, we
				 * can jumble the OID of the function. Otherwise, punt. (Even
				 * if it was a single function call originally, the optimizer
				 * could have simplified it away.)
				 */
				if (list_length(splan->functions) == 1)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) linitial(splan->functions);

					if (IsA(rtfunc->funcexpr, FuncExpr))
					{
						FuncExpr   *funcexpr = (FuncExpr *) rtfunc->funcexpr;
						Oid			funcid = funcexpr->funcid;

						JUMBLE_VALUE(funcid);
					}
				}
			}
			break;
		case T_TableFuncScan:
			{
				TableFuncScan *splan = (TableFuncScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
				/* TODO: Should we jumble splan->tablefunc? */
			}
			break;
		case T_ValuesScan:
			{
				ValuesScan *splan = (ValuesScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
				JumbleNode(jstate, (Node *) splan->values_lists);
			}
			break;
		case T_CteScan:
			{
				CteScan    *splan = (CteScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
			}
			break;
		case T_NamedTuplestoreScan:
			{
				NamedTuplestoreScan *splan = (NamedTuplestoreScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
			}
			break;
		case T_WorkTableScan:
			{
				WorkTableScan *splan = (WorkTableScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
			}
			break;
		case T_ForeignScan:
			{
				ForeignScan *splan = (ForeignScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);

				/*
				 * TODO: Should we jumble any FDW-specific information here,
				 * like EXPLAIN?
				 */
			}
			break;
		case T_CustomScan:
			{
				ForeignScan *splan = (ForeignScan *) plan;

				JumbleScanTarget(jstate, rtable, &splan->scan);
			}
			break;
		case T_NestLoop:
			{
				NestLoop   *jplan = (NestLoop *) plan;

				JUMBLE_VALUE(jplan->join.jointype);
				/* Skip jplan->join.inner_unique */
				JumbleNode(jstate, (Node *) jplan->join.joinqual);
				JumbleNode(jstate, (Node *) jplan->nestParams);
			}
			break;
		case T_MergeJoin:
			{
				MergeJoin  *jplan = (MergeJoin *) plan;
				int			i;

				JUMBLE_VALUE(jplan->join.jointype);
				/* Skip jplan->join.inner_unique */
				JumbleNode(jstate, (Node *) jplan->join.joinqual);
				JumbleNode(jstate, (Node *) jplan->mergeclauses);
				for (i = 0; i < list_length(jplan->mergeclauses); i++)
				{
					JUMBLE_VALUE(jplan->mergeFamilies[i]);
					JUMBLE_VALUE(jplan->mergeCollations[i]);
					JUMBLE_VALUE(jplan->mergeReversals[i]);
					JUMBLE_VALUE(jplan->mergeNullsFirst[i]);
				}
			}
			break;
		case T_HashJoin:
			{
				HashJoin   *jplan = (HashJoin *) plan;

				JUMBLE_VALUE(jplan->join.jointype);
				/* Skip jplan->join.inner_unique */
				JumbleNode(jstate, (Node *) jplan->join.joinqual);
				JumbleNode(jstate, (Node *) jplan->hashclauses);
				JumbleNode(jstate, (Node *) jplan->hashoperators);
				JumbleNode(jstate, (Node *) jplan->hashcollations);
				JumbleNode(jstate, (Node *) jplan->hashkeys);
			}
			break;
		case T_Gather:
			{
				Gather	   *gplan = (Gather *) plan;

				JUMBLE_VALUE(gplan->num_workers);
				/* Skip all other fields */
			}
			break;
		case T_GatherMerge:
			{
				GatherMerge *gplan = (GatherMerge *) plan;

				JUMBLE_VALUE(gplan->num_workers);
				/* Skip all other fields */
			}
			break;
		case T_Hash:
			{
				Hash	   *hplan = (Hash *) plan;

				JumbleNode(jstate, (Node *) hplan->hashkeys);
				/* Skip all other fields */
				break;
			}
			break;
		case T_Memoize:
			{
				Memoize    *mplan = (Memoize *) plan;

				JumbleNode(jstate, (Node *) mplan->param_exprs);
				JUMBLE_VALUE(mplan->binary_mode);
				/* Skip all other fields */
			}
			break;
		case T_Material:
			/* Materialize node has no fields of its own */
			break;
		case T_Sort:
			{
				Sort	   *splan = (Sort *) plan;
				int			i;

				for (i = 0; i < splan->numCols; i++)
				{
					JUMBLE_VALUE(splan->sortColIdx[i]);
					JUMBLE_VALUE(splan->sortOperators[i]);
					JUMBLE_VALUE(splan->collations[i]);
					JUMBLE_VALUE(splan->nullsFirst[i]);
				}
			}
			break;
		case T_IncrementalSort:
			{
				IncrementalSort *splan = (IncrementalSort *) plan;
				int			i;

				for (i = 0; i < splan->sort.numCols; i++)
				{
					JUMBLE_VALUE(splan->sort.sortColIdx[i]);
					JUMBLE_VALUE(splan->sort.sortOperators[i]);
					JUMBLE_VALUE(splan->sort.collations[i]);
					JUMBLE_VALUE(splan->sort.nullsFirst[i]);
				}
				JUMBLE_VALUE(splan->nPresortedCols);
			}
			break;
		case T_Unique:

			/*
			 * Skip all Unique node fields since EXPLAIN does not show them
			 * either
			 */
			break;
		case T_SetOp:
			{
				SetOp	   *splan = (SetOp *) plan;

				JUMBLE_VALUE(splan->cmd);
				JUMBLE_VALUE(splan->strategy);

				/*
				 * Skip all other fields since EXPLAIN does not show them
				 * either
				 */
			}
			break;
		case T_LockRows:

			/*
			 * Skip all LockRows node fields since EXPLAIN does not show them
			 * either
			 */
			break;
		case T_Limit:

			/*
			 * Skip all Limit node fields since EXPLAIN does not show them
			 * either
			 */
			break;
		case T_Agg:
			{
				Agg		   *agg = (Agg *) plan;

				JUMBLE_VALUE(agg->aggstrategy);
				JUMBLE_VALUE(agg->aggsplit);

				/*
				 * Skip all other fields since EXPLAIN does not show them
				 * either
				 */
			}
			break;
		case T_Group:
			{
				Group	   *gplan = (Group *) plan;
				int			i;

				for (i = 0; i < gplan->numCols; i++)
				{
					JUMBLE_VALUE(gplan->grpColIdx[i]);
					JUMBLE_VALUE(gplan->grpOperators[i]);
					JUMBLE_VALUE(gplan->grpCollations[i]);
				}
			}
			break;
		case T_WindowAgg:
			{
				WindowAgg  *wplan = (WindowAgg *) plan;

				JumbleNode(jstate, (Node *) wplan->runConditionOrig);

				/*
				 * Skip all other fields since EXPLAIN does not show them
				 * either
				 */
			}
			break;
		case T_Result:
			{
				Result	   *splan = (Result *) plan;

				JumbleNode(jstate, splan->resconstantqual);
			}
			break;
		case T_ProjectSet:
			/* ProjectSet node has no fields of its own */
			break;
		case T_ModifyTable:
			{
				ModifyTable *splan = (ModifyTable *) plan;
				ListCell   *lc;

				JUMBLE_VALUE(splan->operation);
				foreach(lc, splan->resultRelations)
				{
					JumbleTargetRel(jstate, rtable, plan, lfirst_int(lc));
				}
				JUMBLE_VALUE(splan->onConflictAction);
				foreach(lc, splan->arbiterIndexes)
				{
					JUMBLE_VALUE(lfirst_oid(lc));
				}
				JumbleNode(jstate, splan->onConflictWhere);

				/*
				 * Skip all other fields since EXPLAIN does not show them
				 * either
				 */
			}
			break;
		case T_Append:
			/* Descending into Append node children is handled by the caller */
			break;
		case T_MergeAppend:

			/*
			 * Descending into MergeAppend node children is handled by the
			 * caller
			 */
			break;
		case T_RecursiveUnion:

			/*
			 * Skip all RecursiveUnion node fields since EXPLAIN does not show
			 * them either
			 */
			break;
		case T_BitmapAnd:

			/*
			 * Descending into BitmapAnd node children is handled by the
			 * caller
			 */
			break;
		case T_BitmapOr:
			/* Descending into BitmapOr node children is handled by the caller */
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
			break;
	}
}
