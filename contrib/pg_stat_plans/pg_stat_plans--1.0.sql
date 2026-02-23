/* contrib/pg_stat_plans/pg_stat_plans--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_plans" to load this file. \quit

-- Register functions.
CREATE FUNCTION pg_stat_plans_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION pg_stat_plans(IN showplan boolean,
    OUT userid oid,
    OUT dbid oid,
    OUT toplevel bool,
    OUT queryid bigint,
    OUT planid bigint,
    OUT calls int8,
    OUT total_exec_time float8,
    OUT plan text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_plans_1_0'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- Register a view on the function for ease of use.
CREATE VIEW pg_stat_plans AS
  SELECT * FROM pg_stat_plans(true);

GRANT SELECT ON pg_stat_plans TO PUBLIC;

-- Don't want this to be available to non-superusers.
REVOKE ALL ON FUNCTION pg_stat_plans_reset() FROM PUBLIC;
