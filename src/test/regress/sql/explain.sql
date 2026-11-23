--
-- EXPLAIN
--
-- There are many test cases elsewhere that use EXPLAIN as a vehicle for
-- checking something else (usually planner behavior).  This file is
-- concerned with testing EXPLAIN in its own right.
--

-- To produce stable regression test output, it's usually necessary to
-- ignore details such as exact costs or row counts.  These filter
-- functions replace changeable output details with fixed strings.

create function explain_filter(text) returns setof text
language plpgsql as
$$
declare
    ln text;
begin
    for ln in execute $1
    loop
        -- Replace any numeric word with just 'N'
        ln := regexp_replace(ln, '-?\m\d+\M', 'N', 'g');
        -- In sort output, the above won't match units-suffixed numbers
        ln := regexp_replace(ln, '\m\d+kB', 'NkB', 'g');
        -- Ignore text-mode buffers output because it varies depending
        -- on the system state
        CONTINUE WHEN (ln ~ ' +Buffers: .*');
        -- Ignore text-mode "Planning:" line because whether it's output
        -- varies depending on the system state
        CONTINUE WHEN (ln = 'Planning:');
        return next ln;
    end loop;
end;
$$;

-- To produce valid JSON output, replace numbers with "0" or "0.0" not "N"
create function explain_filter_to_json(text) returns jsonb
language plpgsql as
$$
declare
    data text := '';
    ln text;
begin
    for ln in execute $1
    loop
        -- Replace any numeric word with just '0'
        ln := regexp_replace(ln, '\m\d+\M', '0', 'g');
        data := data || ln;
    end loop;
    return data::jsonb;
end;
$$;

-- Disable JIT, or we'll get different output on machines where that's been
-- forced on
set jit = off;

-- Similarly, disable track_io_timing, to avoid output differences when
-- enabled.
set track_io_timing = off;

-- Simple cases

explain (costs off) select 1 as a, 2 as b having false;
select explain_filter('explain select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers off) select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers off, verbose) select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers, format text) select * from int8_tbl i8');
select explain_filter('explain (buffers, format text) select * from int8_tbl i8');

\a
select explain_filter('explain (analyze, buffers, io, format xml) select * from int8_tbl i8');
select explain_filter('explain (analyze, serialize, buffers, io, format yaml) select * from int8_tbl i8');
select explain_filter('explain (buffers, format json) select * from int8_tbl i8');
\a

-- Index scans track table buffer accesses separately; make sure EXPLAIN
-- (BUFFERS) copes without ANALYZE, i.e. when the plan was never run
select explain_filter('explain (buffers, costs off) select * from tenk1 where unique1 = 42');
\a
select explain_filter('explain (buffers, costs off, format json) select * from tenk1 where unique1 = 42');
\a

-- Check expansion of window definitions

select explain_filter('explain verbose select sum(unique1) over w, sum(unique2) over (w order by hundred), sum(tenthous) over (w order by hundred) from tenk1 window w as (partition by ten)');
select explain_filter('explain verbose select sum(unique1) over w1, sum(unique2) over (w1 order by hundred), sum(tenthous) over (w1 order by hundred rows 10 preceding) from tenk1 window w1 as (partition by ten)');

-- Check output including I/O timings.  These fields are conditional
-- but always set in JSON format, so check them only in this case.
set track_io_timing = on;
select explain_filter('explain (analyze, buffers, format json) select * from int8_tbl i8');
set track_io_timing = off;

-- SETTINGS option
-- We have to ignore other settings that might be imposed by the environment,
-- so printing the whole Settings field unfortunately won't do.

begin;
set local plan_cache_mode = force_generic_plan;
select true as "OK"
  from explain_filter('explain (settings) select * from int8_tbl i8') ln
  where ln ~ '^ *Settings: .*plan_cache_mode = ''force_generic_plan''';
select explain_filter_to_json('explain (settings, format json) select * from int8_tbl i8') #> '{0,Settings,plan_cache_mode}';
rollback;

-- GENERIC_PLAN option

select explain_filter('explain (generic_plan) select unique1 from tenk1 where thousand = $1');
-- should fail
select explain_filter('explain (analyze, generic_plan) select unique1 from tenk1 where thousand = $1');

-- MEMORY option
select explain_filter('explain (memory) select * from int8_tbl i8');
select explain_filter('explain (memory, analyze, buffers off) select * from int8_tbl i8');
select explain_filter('explain (memory, summary, format yaml) select * from int8_tbl i8');
select explain_filter('explain (memory, analyze, format json) select * from int8_tbl i8');
prepare int8_query as select * from int8_tbl i8;
select explain_filter('explain (memory) execute int8_query');

-- Test EXPLAIN (GENERIC_PLAN) with partition pruning
-- partitions should be pruned at plan time, based on constants,
-- but there should be no pruning based on parameter placeholders
create table gen_part (
  key1 integer not null,
  key2 integer not null
) partition by list (key1);
create table gen_part_1
  partition of gen_part for values in (1)
  partition by range (key2);
create table gen_part_1_1
  partition of gen_part_1 for values from (1) to (2);
create table gen_part_1_2
  partition of gen_part_1 for values from (2) to (3);
create table gen_part_2
  partition of gen_part for values in (2);
-- should scan gen_part_1_1 and gen_part_1_2, but not gen_part_2
select explain_filter('explain (generic_plan) select key1, key2 from gen_part where key1 = 1 and key2 = $1');
drop table gen_part;

--
-- Test production of per-worker data
--
-- Unfortunately, because we don't know how many worker processes we'll
-- actually get (maybe none at all), we can't examine the "Workers" output
-- in any detail.  We can check that it parses correctly as JSON, and then
-- remove it from the displayed results.

begin;
-- encourage use of parallel plans
set parallel_setup_cost=0;
set parallel_tuple_cost=0;
set min_parallel_table_scan_size=0;
set max_parallel_workers_per_gather=4;

select jsonb_pretty(
  explain_filter_to_json('explain (analyze, verbose, buffers, format json)
                         select * from tenk1 order by tenthous')
  -- remove "Workers" node of the Seq Scan plan node
  #- '{0,Plan,Plans,0,Plans,0,Workers}'
  -- remove "Workers" node of the Sort plan node
  #- '{0,Plan,Plans,0,Workers}'
  -- Also remove its sort-type fields, as those aren't 100% stable
  #- '{0,Plan,Plans,0,Sort Method}'
  #- '{0,Plan,Plans,0,Sort Space Type}'
);

rollback;

-- Test display of temporary objects
create temp table t1(f1 float8);

create function pg_temp.mysin(float8) returns float8 language plpgsql
as 'begin return sin($1); end';

select explain_filter('explain (verbose) select * from t1 where pg_temp.mysin(f1) < 0.5');

-- Test compute_query_id
set compute_query_id = on;
select explain_filter('explain (verbose) select * from int8_tbl i8');

-- Test compute_query_id with utility statements containing plannable query
select explain_filter('explain (verbose) declare test_cur cursor for select * from int8_tbl');
select explain_filter('explain (verbose) create table test_ctas as select 1');

-- Test SERIALIZE option
select explain_filter('explain (analyze,buffers off,serialize) select * from int8_tbl i8');
select explain_filter('explain (analyze,serialize text,buffers,timing off) select * from int8_tbl i8');
select explain_filter('explain (analyze,serialize binary,buffers,timing) select * from int8_tbl i8');
-- this tests an edge case where we have no data to return
select explain_filter('explain (analyze,buffers off,serialize) create temp table explain_temp as select * from int8_tbl i8');
-- Buffer usage of serialization, here of detoasting the values, should be
-- shown for serialization and not for the plan, which only reads the main
-- table.  So serialization must show at least one local block access per
-- page of the TOAST table, and the plan fewer than that.
create temp table explain_serialize_tab as
  select g as a, (select string_agg(md5((g * 10000 + i)::text), '')
                  from generate_series(1, 1000) i) as t
  from generate_series(1, 10) g;
do $$
declare
    plan_json json;
    toast_pages int;
begin
    select pg_relation_size(reltoastrelid) / current_setting('block_size')::int
      into toast_pages
      from pg_class where oid = 'explain_serialize_tab'::regclass;
    execute 'explain (analyze, serialize, buffers, costs off, format json)
             select * from explain_serialize_tab' into plan_json;
    raise notice 'serialization buffers ok: %, plan buffers ok: %',
        toast_pages > 0 AND
        coalesce((plan_json->0->'Serialization'->>'Local Hit Blocks')::int, 0) +
        coalesce((plan_json->0->'Serialization'->>'Local Read Blocks')::int, 0)
          >= toast_pages,
        coalesce((plan_json->0->'Plan'->>'Local Hit Blocks')::int, 0) +
        coalesce((plan_json->0->'Plan'->>'Local Read Blocks')::int, 0)
          < toast_pages;
end
$$;
drop table explain_serialize_tab;

-- Test tuplestore storage usage in Window aggregate (memory case)
select explain_filter('explain (analyze,buffers off,costs off) select sum(n) over() from generate_series(1,10) a(n)');
-- Test tuplestore storage usage in Window aggregate (disk case)
set work_mem to 64;
select explain_filter('explain (analyze,buffers off,costs off) select sum(n) over() from generate_series(1,2500) a(n)');
-- Test tuplestore storage usage in Window aggregate (memory and disk case, final result is disk)
select explain_filter('explain (analyze,buffers off,costs off) select sum(n) over(partition by m) from (SELECT n < 3 as m, n from generate_series(1,2500) a(n))');

-- Test tuplestore storage usage in Function Scan (memory case)
select explain_filter('explain (analyze,buffers off,costs off) select count(*) from generate_series(1,10) a(n)');
-- Test tuplestore storage usage in Function Scan (disk case)
select explain_filter('explain (analyze,buffers off,costs off) select count(*) from generate_series(1,2500) a(n)');
-- Test tuplestore storage usage in Function Scan with ROWS FROM, which uses
-- one tuplestore per function
select explain_filter('explain (analyze,buffers off,costs off) select count(*) from rows from (generate_series(1,2500), generate_series(1,2500)) a(n,m)');
reset work_mem;

-- EXPLAIN (ANALYZE, BUFFERS) should report buffer usage from PL/pgSQL
-- EXCEPTION blocks, even after subtransaction rollback.
CREATE TEMP TABLE explain_exc_tab (a int, b char(20));
INSERT INTO explain_exc_tab VALUES (0, 'zzz');

CREATE FUNCTION explain_exc_func() RETURNS void AS $$
DECLARE
    v int;
BEGIN
    WITH ins AS (INSERT INTO explain_exc_tab VALUES (1, 'aaa') RETURNING a)
    SELECT a / 0 INTO v FROM ins;
EXCEPTION WHEN division_by_zero THEN
    NULL;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION check_explain_exception_buffers() RETURNS boolean AS $$
DECLARE
    plan_json json;
    node json;
    total_buffers int;
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT explain_exc_func()' INTO plan_json;
    node := plan_json->0->'Plan';
    total_buffers :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);
    RETURN total_buffers > 0;
END;
$$ LANGUAGE plpgsql;

SELECT check_explain_exception_buffers() AS exception_buffers_visible;

-- Also test with nested EXPLAIN ANALYZE (two levels of instrumentation)
CREATE FUNCTION check_explain_exception_buffers_nested() RETURNS boolean AS $$
DECLARE
    plan_json json;
    node json;
    total_buffers int;
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT check_explain_exception_buffers()' INTO plan_json;
    node := plan_json->0->'Plan';
    total_buffers :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);
    RETURN total_buffers > 0;
END;
$$ LANGUAGE plpgsql;

SELECT check_explain_exception_buffers_nested() AS exception_buffers_nested_visible;

DROP FUNCTION check_explain_exception_buffers_nested;
DROP FUNCTION check_explain_exception_buffers;
DROP FUNCTION explain_exc_func;
DROP TABLE explain_exc_tab;

-- Cursor instrumentation test.
-- Verify that buffer usage is correctly tracked through cursor execution paths.
-- Non-scrollable cursors exercise ExecShutdownNode after each ExecutorRun
-- (EXEC_FLAG_BACKWARD is not set), while for scrollable cursors
-- ExecShutdownNode never runs and nodes are only cleaned up in ExecutorEnd.
-- In both cases, buffer usage from the inner cursor scan should be
-- correctly reported.  The table is temporary, so that its local buffer
-- accesses can be told apart from the shared buffer accesses of catalog
-- lookups, and compared exactly.

CREATE TEMP TABLE cursor_buf_test AS SELECT * FROM tenk1;

CREATE FUNCTION cursor_noscroll_scan() RETURNS bigint AS $$
DECLARE
    cur NO SCROLL CURSOR FOR SELECT * FROM cursor_buf_test;
    rec RECORD;
    cnt bigint := 0;
BEGIN
    OPEN cur;
    LOOP
        FETCH NEXT FROM cur INTO rec;
        EXIT WHEN NOT FOUND;
        cnt := cnt + 1;
    END LOOP;
    CLOSE cur;
    RETURN cnt;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION cursor_scroll_scan() RETURNS bigint AS $$
DECLARE
    cur SCROLL CURSOR FOR SELECT * FROM cursor_buf_test;
    rec RECORD;
    cnt bigint := 0;
BEGIN
    OPEN cur;
    LOOP
        FETCH NEXT FROM cur INTO rec;
        EXIT WHEN NOT FOUND;
        cnt := cnt + 1;
    END LOOP;
    CLOSE cur;
    RETURN cnt;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION check_cursor_explain_buffers() RETURNS TABLE(noscroll_ok boolean, scroll_ok boolean) AS $$
DECLARE
    plan_json json;
    node json;
    direct_buf int;
    noscroll_buf int;
    scroll_buf int;
    pages int;
BEGIN
    pages := pg_relation_size('cursor_buf_test') / current_setting('block_size')::int;

    -- Direct scan: get leaf Seq Scan node buffers.  As the first scan of the
    -- table this can access more blocks than the table has pages, since it
    -- also sets hint bits and updates the visibility map, which the later
    -- scans then do not have to repeat.
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT * FROM cursor_buf_test' INTO plan_json;
    node := plan_json->0->'Plan';
    WHILE node->'Plans' IS NOT NULL LOOP
        node := node->'Plans'->0;
    END LOOP;
    direct_buf :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);

    -- Non-scrollable cursor path: ExecShutdownNode runs after each ExecutorRun
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT cursor_noscroll_scan()' INTO plan_json;
    node := plan_json->0->'Plan';
    noscroll_buf :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);

    -- Scrollable cursor path: ExecShutdownNode is skipped
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT cursor_scroll_scan()' INTO plan_json;
    node := plan_json->0->'Plan';
    scroll_buf :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);

    -- Both cursor paths should report exactly one local buffer access per
    -- page of the table, neither lost nor double-counted
    RETURN QUERY SELECT
        (direct_buf >= pages AND noscroll_buf = pages),
        (direct_buf >= pages AND scroll_buf = pages);
END;
$$ LANGUAGE plpgsql;

SELECT * FROM check_cursor_explain_buffers();

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

-- Buffer usage of an EXPLAIN ANALYZE that fails after scanning the whole
-- table, with the error caught by an exception block, should be included in
-- the statement that ran it: at least one local block access per page, and
-- not twice that.  (Bounds rather than exact counts, to stay clear of
-- effects such as read-ahead.)
CREATE FUNCTION explain_fail_after_scan() RETURNS void AS $$
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS)
        SELECT 1 / (count(*) - 10000) FROM cursor_buf_test';
EXCEPTION WHEN division_by_zero THEN
    NULL;
END;
$$ LANGUAGE plpgsql;

SELECT explain_local_blocks('SELECT explain_fail_after_scan()')
    BETWEEN pages AND 2 * pages - 1 AS failed_explain_buffers_ok
FROM (SELECT pg_relation_size('cursor_buf_test') /
      current_setting('block_size')::int AS pages) s;

DROP FUNCTION explain_fail_after_scan;
DROP FUNCTION explain_local_blocks;
DROP FUNCTION check_cursor_explain_buffers;
DROP FUNCTION cursor_noscroll_scan;
DROP FUNCTION cursor_scroll_scan;
DROP TABLE cursor_buf_test;

-- Test buffer usage of CTEs.
-- The subplan of a CTE is printed as an InitPlan under the node it is attached
-- to.  Like that of any child, its buffer usage should be included there and
-- in the nodes above, once, and not in the CTE Scan(s) reading from it, nor in
-- the nodes between them and the node the CTE is attached to.
CREATE TEMP TABLE cte_buf_test AS SELECT a FROM generate_series(1, 1000) a;

CREATE FUNCTION check_cte_explain_buffers(query text,
    OUT cte_scans int, OUT on_cte_scan boolean, OUT on_parent boolean,
    OUT on_top boolean) AS $$
DECLARE
    plan jsonb;
    seq_buf int;
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON) ' || query
        INTO plan;
    plan := plan->0->'Plan';

    SELECT (n->>'Local Hit Blocks')::int + (n->>'Local Read Blocks')::int
        INTO seq_buf
        FROM jsonb_path_query(plan, 'strict $.**?(@."Node Type" == "Seq Scan")') n;

    SELECT count(*),
           bool_or((n->>'Local Hit Blocks')::int + (n->>'Local Read Blocks')::int > 0)
        INTO cte_scans, on_cte_scan
        FROM jsonb_path_query(plan, 'strict $.**?(@."Node Type" == "CTE Scan")') n;

    -- The node between the top node, which the CTE is attached to, and the
    -- CTE Scan(s)
    SELECT (n->>'Local Hit Blocks')::int + (n->>'Local Read Blocks')::int > 0
        INTO on_parent
        FROM jsonb_path_query(plan, '$.Plans[*]?(@."Parent Relationship" != "InitPlan")') n;

    on_top := seq_buf > 0 AND
        (plan->>'Local Hit Blocks')::int + (plan->>'Local Read Blocks')::int = seq_buf;
END;
$$ LANGUAGE plpgsql;

SELECT * FROM check_cte_explain_buffers($$
    WITH c AS MATERIALIZED (SELECT * FROM cte_buf_test)
    SELECT * FROM c ORDER BY a LIMIT 1$$);
SELECT * FROM check_cte_explain_buffers($$
    WITH c AS MATERIALIZED (SELECT * FROM cte_buf_test)
    SELECT * FROM (SELECT count(*) FROM c c1, c c2 WHERE c1.a = c2.a) s LIMIT 1$$);

DROP FUNCTION check_cte_explain_buffers;
DROP TABLE cte_buf_test;

-- Test trigger instrumentation.
-- The trigger scans a multi-page temporary table, so wherever its buffer
-- usage is reported, there must be at least one local block access per page
-- of it.
CREATE TEMP TABLE trig_test_tab (a int);
CREATE TEMP TABLE trig_work_tab (a int);
INSERT INTO trig_work_tab SELECT generate_series(1, 10000);

CREATE FUNCTION trig_test_func() RETURNS trigger AS $$
BEGIN
    PERFORM * FROM trig_work_tab;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trig_test_trig
    BEFORE INSERT ON trig_test_tab
    FOR EACH ROW EXECUTE FUNCTION trig_test_func();

-- The buffer usage of a trigger is reported for the trigger itself, whether
-- it is a BEFORE or an AFTER trigger, and is not part of any plan node, not
-- even the ModifyTable node that fires BEFORE triggers.
CREATE FUNCTION check_trigger_own_buffers(
    OUT calls int, OUT on_trigger boolean, OUT on_node boolean) AS $$
DECLARE
    plan_json json;
    node json;
    trig json;
    pages int;
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        INSERT INTO trig_test_tab VALUES (1)' INTO plan_json;
    node := plan_json->0->'Plan';
    trig := plan_json->0->'Triggers'->0;
    pages := pg_relation_size('trig_work_tab') / current_setting('block_size')::int;
    calls := (trig->>'Calls')::int;
    on_trigger :=
        COALESCE((trig->>'Local Hit Blocks')::int, 0) +
        COALESCE((trig->>'Local Read Blocks')::int, 0) >= pages;
    on_node :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0) >= pages;
END;
$$ LANGUAGE plpgsql;

SELECT * FROM check_trigger_own_buffers();

DROP TRIGGER trig_test_trig ON trig_test_tab;
CREATE TRIGGER trig_test_trig
    AFTER INSERT ON trig_test_tab
    FOR EACH ROW EXECUTE FUNCTION trig_test_func();

SELECT * FROM check_trigger_own_buffers();

DROP FUNCTION check_trigger_own_buffers;
DROP TRIGGER trig_test_trig ON trig_test_tab;
DROP FUNCTION trig_test_func;
DROP TABLE trig_test_tab;
DROP TABLE trig_work_tab;

-- EXPLAIN (ANALYZE, BUFFERS) should report buffer usage of parallel workers
-- launched by a nested statement on the plan node that ran the function.
-- With the leader not participating, the workers do all the scanning, so the
-- node must show at least one block access per page of the table (given a
-- sequential scan, which is forced so that the page count is a known
-- quantity).
CREATE FUNCTION nested_parallel_count() RETURNS bigint AS $$
BEGIN
    RETURN (SELECT count(*) FROM tenk1);
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION check_nested_parallel_buffers() RETURNS boolean AS $$
DECLARE
    plan_json json;
    node json;
    node_buf int;
    pages int;
BEGIN
    SET LOCAL enable_indexscan = off;
    SET LOCAL enable_indexonlyscan = off;
    SET LOCAL enable_bitmapscan = off;
    SET LOCAL parallel_setup_cost = 0;
    SET LOCAL parallel_tuple_cost = 0;
    SET LOCAL min_parallel_table_scan_size = 0;
    SET LOCAL max_parallel_workers_per_gather = 2;
    SET LOCAL parallel_leader_participation = off;
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT nested_parallel_count()' INTO plan_json;
    node := plan_json->0->'Plan';
    node_buf :=
        COALESCE((node->>'Shared Hit Blocks')::int, 0) +
        COALESCE((node->>'Shared Read Blocks')::int, 0);
    pages := pg_relation_size('tenk1') / current_setting('block_size')::int;
    RETURN node_buf >= pages;
END;
$$ LANGUAGE plpgsql;

SELECT check_nested_parallel_buffers() AS nested_parallel_buffers_on_node;

DROP FUNCTION check_nested_parallel_buffers;
DROP FUNCTION nested_parallel_count;

-- Likewise, EXPLAIN (ANALYZE, BUFFERS) should report the buffer usage of
-- parallel workers launched by a statement a trigger ran with the trigger.
CREATE TEMP TABLE trig_par_tab (a int);

CREATE FUNCTION trig_par_func() RETURNS trigger AS $$
BEGIN
    PERFORM count(*) FROM tenk1;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trig_par_trig
    BEFORE INSERT ON trig_par_tab
    FOR EACH ROW EXECUTE FUNCTION trig_par_func();

CREATE FUNCTION check_trigger_parallel_buffers() RETURNS boolean AS $$
DECLARE
    plan_json json;
    trig json;
    trig_buf int;
    pages int;
BEGIN
    SET LOCAL enable_indexscan = off;
    SET LOCAL enable_indexonlyscan = off;
    SET LOCAL enable_bitmapscan = off;
    SET LOCAL parallel_setup_cost = 0;
    SET LOCAL parallel_tuple_cost = 0;
    SET LOCAL min_parallel_table_scan_size = 0;
    SET LOCAL max_parallel_workers_per_gather = 2;
    SET LOCAL parallel_leader_participation = off;
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        INSERT INTO trig_par_tab VALUES (1)' INTO plan_json;
    trig := plan_json->0->'Triggers'->0;
    trig_buf :=
        COALESCE((trig->>'Shared Hit Blocks')::int, 0) +
        COALESCE((trig->>'Shared Read Blocks')::int, 0);
    pages := pg_relation_size('tenk1') / current_setting('block_size')::int;
    RETURN trig_buf >= pages;
END;
$$ LANGUAGE plpgsql;

SELECT check_trigger_parallel_buffers() AS trigger_parallel_buffers;

DROP FUNCTION check_trigger_parallel_buffers;
DROP TRIGGER trig_par_trig ON trig_par_tab;
DROP FUNCTION trig_par_func;
DROP TABLE trig_par_tab;

-- EXPLAIN (ANALYZE, BUFFERS) should report the buffer usage of a parallel
-- scan on the scan node, combined across the leader and its workers.  Each
-- page of the table is scanned by exactly one process, so with a sequential
-- scan the node must show exactly one block access per page, however many
-- workers were launched.  The Gather node above includes its children.
CREATE FUNCTION check_parallel_scan_buffers()
    RETURNS TABLE(is_parallel_scan boolean, scan_ok boolean, gather_ok boolean) AS $$
DECLARE
    plan_json json;
    node json;
    gather json;
    scan_buf int;
    gather_buf int;
    pages int;
BEGIN
    SET LOCAL enable_indexscan = off;
    SET LOCAL enable_indexonlyscan = off;
    SET LOCAL enable_bitmapscan = off;
    SET LOCAL parallel_setup_cost = 0;
    SET LOCAL parallel_tuple_cost = 0;
    SET LOCAL min_parallel_table_scan_size = 0;
    SET LOCAL max_parallel_workers_per_gather = 2;
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT count(*) FROM tenk1' INTO plan_json;
    node := plan_json->0->'Plan';
    WHILE node->'Plans' IS NOT NULL LOOP
        IF node->>'Node Type' = 'Gather' THEN
            gather := node;
        END IF;
        node := node->'Plans'->0;
    END LOOP;
    scan_buf :=
        COALESCE((node->>'Shared Hit Blocks')::int, 0) +
        COALESCE((node->>'Shared Read Blocks')::int, 0);
    gather_buf :=
        COALESCE((gather->>'Shared Hit Blocks')::int, 0) +
        COALESCE((gather->>'Shared Read Blocks')::int, 0);
    pages := pg_relation_size('tenk1') / current_setting('block_size')::int;
    RETURN QUERY SELECT
        (node->>'Node Type' = 'Seq Scan' AND (node->>'Parallel Aware')::boolean),
        scan_buf = pages,
        gather_buf >= scan_buf;
END;
$$ LANGUAGE plpgsql;

SELECT * FROM check_parallel_scan_buffers();

DROP FUNCTION check_parallel_scan_buffers;

-- Parallel index scan buffer usage.
-- Table accesses of an index scan are tracked separately from the index
-- accesses, and parallel workers report them to the leader separately as
-- well.  Verify that the total shown for the scan node matches between a
-- serial and a parallel execution where only workers do the scanning.
CREATE TABLE par_idx_buf_test AS SELECT * FROM tenk1;
CREATE INDEX ON par_idx_buf_test (unique1);
VACUUM ANALYZE par_idx_buf_test;

CREATE FUNCTION check_parallel_indexscan_buffers() RETURNS boolean AS $$
DECLARE
    plan_json json;
    node json;
    serial_buf int;
    parallel_buf int;
BEGIN
    -- Serial index scan: get the leaf scan node buffers as baseline
    SET LOCAL max_parallel_workers_per_gather = 0;
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT count(stringu1) FROM par_idx_buf_test WHERE unique1 >= 0' INTO plan_json;
    node := plan_json->0->'Plan';
    WHILE node->'Plans' IS NOT NULL LOOP
        node := node->'Plans'->0;
    END LOOP;
    IF node->>'Node Type' <> 'Index Scan' THEN
        RAISE EXCEPTION 'expected Index Scan, got %', node->>'Node Type';
    END IF;
    serial_buf :=
        COALESCE((node->>'Shared Hit Blocks')::int, 0) +
        COALESCE((node->>'Shared Read Blocks')::int, 0);

    -- Parallel index scan with only workers scanning
    SET LOCAL parallel_setup_cost = 0;
    SET LOCAL parallel_tuple_cost = 0;
    SET LOCAL min_parallel_index_scan_size = 0;
    SET LOCAL min_parallel_table_scan_size = 0;
    SET LOCAL max_parallel_workers_per_gather = 2;
    SET LOCAL parallel_leader_participation = off;
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT count(stringu1) FROM par_idx_buf_test WHERE unique1 >= 0' INTO plan_json;
    node := plan_json->0->'Plan';
    WHILE node->'Plans' IS NOT NULL LOOP
        node := node->'Plans'->0;
    END LOOP;
    IF node->>'Node Type' <> 'Index Scan' OR NOT (node->>'Parallel Aware')::bool THEN
        RAISE EXCEPTION 'expected Parallel Index Scan, got %', node->>'Node Type';
    END IF;
    parallel_buf :=
        COALESCE((node->>'Shared Hit Blocks')::int, 0) +
        COALESCE((node->>'Shared Read Blocks')::int, 0);

    -- Workers may each fetch some heap pages the other also fetched, so
    -- allow for a little slack, but the totals must be in the same ballpark.
    RETURN parallel_buf >= serial_buf * 0.9 AND parallel_buf <= serial_buf * 1.5;
END;
$$ LANGUAGE plpgsql;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
SELECT check_parallel_indexscan_buffers() AS parallel_indexscan_buffers_match;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexonlyscan;

DROP FUNCTION check_parallel_indexscan_buffers;
DROP TABLE par_idx_buf_test;
