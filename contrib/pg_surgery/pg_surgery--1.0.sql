/* contrib/pg_surgery/pg_surgery--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_surgery" to load this file. \quit

CREATE FUNCTION heap_force_kill(reloid regclass, tids tid[])
RETURNS VOID
AS 'MODULE_PATHNAME', 'heap_force_kill'
LANGUAGE C STRICT;

CREATE FUNCTION heap_force_freeze(reloid regclass, tids tid[])
RETURNS VOID
AS 'MODULE_PATHNAME', 'heap_force_freeze'
LANGUAGE C STRICT;
