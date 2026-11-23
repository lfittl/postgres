-- Tests for the instrumentation stack: failing to push an entry, because
-- growing the stack fails, must leave the stack and the entries in a state
-- the abort cleanup copes with.
CREATE EXTENSION injection_points;

-- Start a new session, so that the first entry pushed allocates the stack.
\c
SELECT injection_points_set_local();

CREATE TEMP TABLE instr_tab AS SELECT g AS a FROM generate_series(1, 10000) g;
SELECT pg_relation_size('instr_tab') / current_setting('block_size')::int AS pages
\gset

-- Local buffer accesses of the top plan node of an EXPLAIN ANALYZE
CREATE FUNCTION explain_local_blocks(query text) RETURNS int AS $$
DECLARE
    plan_json json;
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON) ' || query
        INTO plan_json;
    RETURN COALESCE((plan_json->0->'Plan'->>'Local Hit Blocks')::int, 0) +
        COALESCE((plan_json->0->'Plan'->>'Local Read Blocks')::int, 0);
END;
$$ LANGUAGE plpgsql;

-- What failed statements left behind, checked between statements:
-- * Instrumentation memory.  A statement's entry is registered with a
--   resource owner before it is pushed, so it must be freed even if it never
--   made it onto the stack.  (That is the case when the first push of the
--   session fails; later, the failing push is that of a plan node.)
-- * Entries on the stack, which must be empty, with the session's entry as
--   the current one.  The EXPLAINs below cannot tell, since the entries they
--   push would go on top of anything left behind.
CREATE TEMP VIEW instr_leftover AS
SELECT count(*) AS leftover_contexts, instr_stack_depth() AS stack_depth
  FROM pg_backend_memory_contexts
  WHERE name = 'Instrumentation';

-- Allocating the stack fails when the first entry is pushed.
SELECT injection_points_attach('instrumentation-stack-grow', 'error');
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
    SELECT count(*) FROM instr_tab;
SELECT * FROM instr_leftover;
SELECT injection_points_detach('instrumentation-stack-grow');
SELECT explain_local_blocks('SELECT count(*) FROM instr_tab')
    BETWEEN :pages AND 2 * :pages - 1 AS buffers_ok;

-- A plan deeper than the initial size of the stack, so that growing it
-- fails with the query entry and several plan node entries on the stack.
CREATE TEMP VIEW instr_deep AS
SELECT count(*) FROM
  (SELECT * FROM (SELECT * FROM (SELECT * FROM (SELECT * FROM
  (SELECT * FROM (SELECT * FROM (SELECT * FROM (SELECT * FROM
  (SELECT * FROM (SELECT * FROM instr_tab LIMIT 100000) s1
  LIMIT 100000) s2 LIMIT 100000) s3 LIMIT 100000) s4 LIMIT 100000) s5
  LIMIT 100000) s6 LIMIT 100000) s7 LIMIT 100000) s8 LIMIT 100000) s9
  LIMIT 100000) s10;
EXPLAIN (COSTS OFF) SELECT * FROM instr_deep;

SELECT injection_points_attach('instrumentation-stack-grow', 'error');
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
    SELECT * FROM instr_deep;
SELECT * FROM instr_leftover;

-- Same, with the error caught in a nested statement, under an outer EXPLAIN
-- ANALYZE that must complete normally.
CREATE FUNCTION instr_deep_caught() RETURNS void AS $$
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM instr_deep';
EXCEPTION WHEN internal_error THEN
    RAISE NOTICE 'caught: %', SQLERRM;
END;
$$ LANGUAGE plpgsql;
SELECT explain_local_blocks('SELECT instr_deep_caught()') >= 0 AS outer_ok;
SELECT * FROM instr_leftover;
SELECT injection_points_detach('instrumentation-stack-grow');

-- The stack works normally afterwards, and grows as needed.
SELECT explain_local_blocks('SELECT count(*) FROM instr_tab')
    BETWEEN :pages AND 2 * :pages - 1 AS buffers_ok;
SELECT explain_local_blocks('SELECT * FROM instr_deep')
    BETWEEN :pages AND 2 * :pages - 1 AS deep_buffers_ok;
SELECT * FROM instr_leftover;

DROP FUNCTION instr_deep_caught;
DROP VIEW instr_deep;
DROP VIEW instr_leftover;
DROP FUNCTION explain_local_blocks;
DROP TABLE instr_tab;
DROP EXTENSION injection_points;
