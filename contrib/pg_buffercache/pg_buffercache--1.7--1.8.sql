/* contrib/pg_buffercache/pg_buffercache--1.7--1.8.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.8'" to load this file. \quit

CREATE FUNCTION pg_buffercache_relation_stats(
    OUT relfilenode oid,
    OUT reltablespace oid,
    OUT reldatabase oid,
    OUT relforknumber int2,
    OUT buffers int4,
    OUT buffers_dirty int4,
    OUT buffers_pinned int4,
    OUT usagecount_avg float8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_buffercache_relation_stats'
LANGUAGE C PARALLEL SAFE;

REVOKE ALL ON FUNCTION pg_buffercache_relation_stats() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_buffercache_relation_stats() TO pg_monitor;
