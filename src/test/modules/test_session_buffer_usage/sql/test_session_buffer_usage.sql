LOAD 'test_session_buffer_usage';
CREATE EXTENSION test_session_buffer_usage;

-- Verify all columns are non-negative
SELECT count(*) = 1 AS ok FROM test_session_buffer_usage()
WHERE shared_blks_hit >= 0 AND shared_blks_read >= 0
  AND shared_blks_dirtied >= 0 AND shared_blks_written >= 0
  AND local_blks_hit >= 0 AND local_blks_read >= 0
  AND local_blks_dirtied >= 0 AND local_blks_written >= 0
  AND temp_blks_read >= 0 AND temp_blks_written >= 0
  AND shared_blk_read_time >= 0 AND shared_blk_write_time >= 0
  AND local_blk_read_time >= 0 AND local_blk_write_time >= 0
  AND temp_blk_read_time >= 0 AND temp_blk_write_time >= 0;

-- Verify counters increase after buffer activity
SELECT test_session_buffer_usage_reset();

CREATE TEMP TABLE test_buf_activity (id int, data text);
INSERT INTO test_buf_activity SELECT i, repeat('x', 100) FROM generate_series(1, 1000) AS i;
SELECT count(*) FROM test_buf_activity;

SELECT local_blks_hit + local_blks_read > 0 AS blocks_increased
FROM test_session_buffer_usage();

DROP TABLE test_buf_activity;

-- Parallel query test
--
-- The tables of the parallel tests are sized so that scanning them dominates
-- the leader's own catalog accesses by a wide margin: with
-- RELCACHE_FORCE_RELEASE, every relation open rebuilds the relcache entry
-- from the catalogs, which costs the leader a few hundred buffer hits per
-- parallel query.
CREATE TABLE par_dc_tab (a int, b char(200));
INSERT INTO par_dc_tab SELECT i, repeat('x', 200) FROM generate_series(1, 100000) AS i;

SELECT count(*) FROM par_dc_tab;

-- Measure serial scan delta (leader does all the work)
SET max_parallel_workers_per_gather = 0;

SELECT test_session_buffer_usage_reset();
SELECT count(*) FROM par_dc_tab;

CREATE TEMP TABLE dc_serial_result AS
SELECT shared_blks_hit AS serial_delta FROM test_session_buffer_usage();

-- Measure parallel scan delta with leader NOT participating in scanning.
-- Workers do all table scanning; leader only runs the Gather node.
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET parallel_leader_participation = off;

SELECT test_session_buffer_usage_reset();
SELECT count(*) FROM par_dc_tab;

-- The session totals only describe the leader's own activity, so the scan
-- done by the workers must not show up here.
SELECT shared_blks_hit < s.serial_delta / 2 AS leader_excludes_workers
FROM test_session_buffer_usage(), dc_serial_result s;

RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;
RESET parallel_leader_participation;

DROP TABLE par_dc_tab, dc_serial_result;

--
-- Abort/exception tests: verify buffer usage survives various error paths.
--

-- Rolled-back divide-by-zero under EXPLAIN ANALYZE
CREATE TEMP TABLE exc_tab (a int, b char(20));

SELECT test_session_buffer_usage_reset();

EXPLAIN (ANALYZE, BUFFERS, COSTS OFF)
        WITH ins AS (INSERT INTO exc_tab VALUES (1, 'aaa') RETURNING a)
        SELECT a / 0 FROM ins;

SELECT local_blks_dirtied > 0 AS exception_buffers_visible
FROM test_session_buffer_usage();

DROP TABLE exc_tab;

-- Unique constraint violation in regular query
CREATE TEMP TABLE unique_tab (a int UNIQUE, b char(20));
INSERT INTO unique_tab VALUES (1, 'first');

SELECT test_session_buffer_usage_reset();
INSERT INTO unique_tab VALUES (1, 'duplicate');

SELECT local_blks_hit > 0 AS unique_violation_buffers_visible
FROM test_session_buffer_usage();

DROP TABLE unique_tab;

-- Caught exception in PL/pgSQL subtransaction (BEGIN...EXCEPTION)
CREATE TEMP TABLE subxact_tab (a int, b char(20));

CREATE FUNCTION subxact_exc_func() RETURNS text AS $$
BEGIN
    BEGIN
        EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF)
            WITH ins AS (INSERT INTO subxact_tab VALUES (1, ''aaa'') RETURNING a)
            SELECT a / 0 FROM ins';
    EXCEPTION WHEN division_by_zero THEN
        RETURN 'caught';
    END;
    RETURN 'not reached';
END;
$$ LANGUAGE plpgsql;

SELECT test_session_buffer_usage_reset();
SELECT subxact_exc_func();

SELECT local_blks_dirtied > 0 AS subxact_buffers_visible
FROM test_session_buffer_usage();

DROP FUNCTION subxact_exc_func;
DROP TABLE subxact_tab;

-- Cursor (FOR loop) in aborted subtransaction; verify post-exception tracking
CREATE TEMP TABLE cursor_tab (a int, b char(200));
INSERT INTO cursor_tab SELECT i, repeat('x', 200) FROM generate_series(1, 500) AS i;

CREATE FUNCTION cursor_exc_func() RETURNS text AS $$
DECLARE
    rec record;
    cnt int := 0;
BEGIN
    BEGIN
        FOR rec IN SELECT * FROM cursor_tab LOOP
            cnt := cnt + 1;
            IF cnt = 250 THEN
                PERFORM 1 / 0;
            END IF;
        END LOOP;
    EXCEPTION WHEN division_by_zero THEN
        RETURN 'caught after ' || cnt || ' rows';
    END;
    RETURN 'not reached';
END;
$$ LANGUAGE plpgsql;

SELECT test_session_buffer_usage_reset();
SELECT cursor_exc_func();

SELECT local_blks_hit + local_blks_read > 0
       AS cursor_subxact_buffers_visible
FROM test_session_buffer_usage();

DROP FUNCTION cursor_exc_func;
DROP TABLE cursor_tab;

-- Trigger abort under EXPLAIN ANALYZE: verify that buffer activity from a
-- trigger that throws an error is still properly propagated.
CREATE TEMP TABLE trig_err_tab (a int);
CREATE TEMP TABLE trig_work_tab (a int, b char(200));
INSERT INTO trig_work_tab SELECT i, repeat('x', 200) FROM generate_series(1, 500) AS i;

-- Warm local buffers so trig_work_tab reads become hits
SELECT count(*) FROM trig_work_tab;

CREATE FUNCTION trig_err_func() RETURNS trigger AS $$
BEGIN
    PERFORM count(*) FROM trig_work_tab;
    RAISE EXCEPTION 'trigger error';
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trig_err BEFORE INSERT ON trig_err_tab
    FOR EACH ROW EXECUTE FUNCTION trig_err_func();

-- Measure how many local buffer hits a scan of trig_work_tab produces
SELECT test_session_buffer_usage_reset();
SELECT count(*) FROM trig_work_tab;

CREATE TEMP TABLE trig_serial_result AS
SELECT local_blks_hit AS serial_hits FROM test_session_buffer_usage();

-- Now trigger the same scan via a trigger that errors
SELECT test_session_buffer_usage_reset();
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF)
    INSERT INTO trig_err_tab VALUES (1);

-- The trigger scanned trig_work_tab but errored before InstrStopTrigger ran,
-- so its entry was never finalized.  The query's QueryInstrumentation keeps
-- such entries in its unfinalized list, and the resource owner cleanup on
-- abort accumulates them, so the buffer data is still propagated.
SELECT local_blks_hit >= s.serial_hits / 2
       AS trigger_abort_buffers_propagated
FROM test_session_buffer_usage(), trig_serial_result s;

DROP TABLE trig_err_tab, trig_work_tab, trig_serial_result;
DROP FUNCTION trig_err_func;

-- Parallel worker abort.  (The failing row is near the end of the table, so
-- that the workers scan most of it before the error.)
--
-- The session totals only ever describe the leader's own activity; what the
-- workers did is reported by the workers themselves.  That must hold when a
-- worker errors out as well: the error unwinds the leader's query entry
-- through the resource owner, and nothing of the workers' activity may leak
-- into the leader's totals, nor may the leader's instrumentation stop
-- working afterwards.
CREATE TABLE par_abort_tab (a int, b char(200));
INSERT INTO par_abort_tab SELECT i, repeat('x', 200) FROM generate_series(1, 100000) AS i;

-- Warm shared buffers so all reads become hits
SELECT count(*) FROM par_abort_tab;

-- Measure serial scan delta as a reference (leader reads all blocks)
SET max_parallel_workers_per_gather = 0;

SELECT test_session_buffer_usage_reset();
SELECT b::int2 FROM par_abort_tab WHERE a > 99000;

CREATE TABLE par_abort_serial_result AS
SELECT shared_blks_hit AS serial_delta FROM test_session_buffer_usage();

-- Now force parallel with leader NOT participating in scanning
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET parallel_leader_participation = off;
SET debug_parallel_query = on; -- Ensure we get CONTEXT line consistently

SELECT test_session_buffer_usage_reset();
SELECT b::int2 FROM par_abort_tab WHERE a > 99000;

RESET debug_parallel_query;

-- The workers scanned the table, but the leader's delta must be far less
-- than a serial scan: the session totals only describe the leader's own
-- activity, also after a worker error.
SELECT shared_blks_hit < s.serial_delta / 2
       AS worker_abort_leader_totals_only
FROM test_session_buffer_usage(), par_abort_serial_result s;

-- The leader's own instrumentation must be intact after the error: a serial
-- full scan is tracked in full again, i.e. at least one hit per page.
SET max_parallel_workers_per_gather = 0;
SELECT test_session_buffer_usage_reset();
SELECT count(*) FROM par_abort_tab;
CREATE TABLE par_abort_after_result AS
SELECT shared_blks_hit AS count_delta FROM test_session_buffer_usage();
SELECT count_delta >= pg_relation_size('par_abort_tab') / current_setting('block_size')::int
       AS tracking_intact_after_worker_abort
FROM par_abort_after_result;

RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;
RESET parallel_leader_participation;

DROP TABLE par_abort_tab, par_abort_serial_result, par_abort_after_result;

-- Cursor fetched inside a transaction that then aborts, with query-level
-- instrumentation on the cursor's own query (as it would be with
-- pg_stat_statements loaded).
--
-- The fetches accumulate into the cursor query's QueryInstrumentation, which
-- only reaches the session totals when it is finalized: at ExecutorFinish in
-- the normal case, or through the resource owner cleanup on abort.  Here the
-- portal is dropped as failed without ExecutorFinish ever running, so the
-- buffer activity of the fetches must be recovered by the abort cleanup.
SET test_session_buffer_usage.track_queries = on;

CREATE TABLE cursor_abort_tab (a int, b char(200));
INSERT INTO cursor_abort_tab SELECT i, repeat('x', 200) FROM generate_series(1, 5000) AS i;

-- Warm shared buffers so all reads become hits
SELECT count(*) FROM cursor_abort_tab;

-- Reference: a complete scan outside of a cursor
SELECT test_session_buffer_usage_reset();
SELECT count(*) FROM cursor_abort_tab;

CREATE TABLE cursor_abort_serial_result AS
SELECT shared_blks_hit AS serial_delta FROM test_session_buffer_usage();

-- Same scan through a cursor, in a transaction that aborts after the fetch
SELECT test_session_buffer_usage_reset();
BEGIN;
DECLARE cursor_abort_cur CURSOR FOR SELECT * FROM cursor_abort_tab;
MOVE ALL IN cursor_abort_cur;
SELECT 1 / 0;
ROLLBACK;

SELECT shared_blks_hit >= s.serial_delta / 2
       AS cursor_abort_buffers_visible
FROM test_session_buffer_usage(), cursor_abort_serial_result s;

RESET test_session_buffer_usage.track_queries;
DROP TABLE cursor_abort_tab, cursor_abort_serial_result;

-- Cleanup
DROP EXTENSION test_session_buffer_usage;
