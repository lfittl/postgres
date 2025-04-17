/* contrib/pg_stat_statements/pg_stat_statements--1.12--1.13.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.13'" to load this file. \quit

/* First we have to remove them from the extension */
ALTER EXTENSION pg_stat_statements DROP VIEW pg_stat_statements_info;
ALTER EXTENSION pg_stat_statements DROP FUNCTION pg_stat_statements_info();

/* Then we can drop them */
DROP VIEW pg_stat_statements_info;
DROP FUNCTION pg_stat_statements_info();

/* Now redefine */
CREATE FUNCTION pg_stat_statements_info(
    OUT dealloc bigint,
    OUT gc_count bigint,
    OUT query_file_size bigint,
    OUT stats_reset timestamp with time zone
)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE VIEW pg_stat_statements_info AS
  SELECT * FROM pg_stat_statements_info();

GRANT SELECT ON pg_stat_statements_info TO PUBLIC;
