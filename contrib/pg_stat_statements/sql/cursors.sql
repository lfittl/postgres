--
-- Cursors
--

-- These tests require track_utility to be enabled.
SET pg_stat_statements.track_utility = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- DECLARE
-- SELECT is normalized.
DECLARE cursor_stats_1 CURSOR WITH HOLD FOR SELECT 1;
CLOSE cursor_stats_1;
DECLARE cursor_stats_1 CURSOR WITH HOLD FOR SELECT 2;
CLOSE cursor_stats_1;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- FETCH
BEGIN;
DECLARE cursor_stats_1 CURSOR WITH HOLD FOR SELECT 2;
DECLARE cursor_stats_2 CURSOR WITH HOLD FOR SELECT 3;
FETCH 1 IN cursor_stats_1;
FETCH 1 IN cursor_stats_2;
CLOSE cursor_stats_1;
CLOSE cursor_stats_2;
COMMIT;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Normalization of FETCH statements
BEGIN;
DECLARE pgss_cursor CURSOR FOR SELECT FROM generate_series(1, 10);
-- implicit directions
FETCH pgss_cursor;
FETCH 1 pgss_cursor;
FETCH 2 pgss_cursor;
FETCH -1 pgss_cursor;
-- explicit NEXT
FETCH NEXT pgss_cursor;
-- explicit PRIOR
FETCH PRIOR pgss_cursor;
-- explicit FIRST
FETCH FIRST pgss_cursor;
-- explicit LAST
FETCH LAST pgss_cursor;
-- explicit ABSOLUTE
FETCH ABSOLUTE 1 pgss_cursor;
FETCH ABSOLUTE 2 pgss_cursor;
FETCH ABSOLUTE -1 pgss_cursor;
-- explicit RELATIVE
FETCH RELATIVE 1 pgss_cursor;
FETCH RELATIVE 2 pgss_cursor;
FETCH RELATIVE -1 pgss_cursor;
-- explicit FORWARD
FETCH ALL pgss_cursor;
-- explicit FORWARD ALL
FETCH FORWARD ALL pgss_cursor;
-- explicit FETCH FORWARD
FETCH FORWARD pgss_cursor;
FETCH FORWARD 1 pgss_cursor;
FETCH FORWARD 2 pgss_cursor;
FETCH FORWARD -1 pgss_cursor;
-- explicit FETCH BACKWARD
FETCH BACKWARD pgss_cursor;
FETCH BACKWARD 1 pgss_cursor;
FETCH BACKWARD 2 pgss_cursor;
FETCH BACKWARD -1 pgss_cursor;
-- explicit BACKWARD ALL
FETCH BACKWARD ALL pgss_cursor;
COMMIT;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Buffer usage of a cursor's query is attributed to the statements that
-- fetch from it, rather than to the one that closes the cursor.
SET pg_stat_statements.track = 'all';
CREATE TEMP TABLE pgss_cursor_tab AS SELECT a FROM generate_series(1, 1000) a;
-- Scan the table once, so that the scan below does not have to set hint bits
SELECT count(*) FROM pgss_cursor_tab;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
BEGIN;
DECLARE pgss_cursor CURSOR FOR SELECT FROM pgss_cursor_tab;
FETCH 500 pgss_cursor;
FETCH ALL pgss_cursor;
CLOSE pgss_cursor;
COMMIT;
SELECT query, calls, local_blks_hit + local_blks_read > 0 AS local_blks
  FROM pg_stat_statements
  WHERE query LIKE 'FETCH%' OR query LIKE 'CLOSE%' OR query LIKE 'COMMIT%'
  ORDER BY query COLLATE "C";
SELECT sum(local_blks_hit + local_blks_read) =
       pg_relation_size('pgss_cursor_tab') / current_setting('block_size')::int
       AS fetches_cover_table
  FROM pg_stat_statements
  WHERE query LIKE 'FETCH%';
DROP TABLE pgss_cursor_tab;
RESET pg_stat_statements.track;
