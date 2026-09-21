/*
 * PostgreSQL System Views
 *
 * Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * src/backend/catalog/system_views.sql
 *
 * Note: this file is read in single-user -j mode, which means that the
 * command terminator is semicolon-newline-newline; whenever the backend
 * sees that, it stops and executes what it's got.  If you write a lot of
 * statements without empty lines between, they'll all get quoted to you
 * in any error message about one of them, so don't do that.  Also, you
 * cannot write a semicolon immediately followed by an empty line in a
 * string literal (including a function body!) or a multiline comment.
 */


CREATE VIEW pg_views AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS viewname,
        pg_get_viewdef(C.oid) AS definition
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.relkind = 'v';

CREATE VIEW pg_tables AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS tablename,
        T.spcname AS tablespace,
        C.relhasindex AS hasindexes,
        C.relhasrules AS hasrules
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
         LEFT JOIN pg_tablespace T ON (T.oid = C.reltablespace)
    WHERE C.relkind IN ('r');

CREATE VIEW pg_indexes AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS tablename,
        I.relname AS indexname,
        T.spcname AS tablespace,
        pg_get_indexdef(I.oid) AS indexdef
    FROM pg_index X JOIN pg_class C ON (C.oid = X.indrelid)
         JOIN pg_class I ON (I.oid = X.indexrelid)
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
         LEFT JOIN pg_tablespace T ON (T.oid = I.reltablespace)
    WHERE C.relkind IN ('r') AND I.relkind IN ('i', 'I');

CREATE VIEW pg_stats AS
    SELECT
        nspname AS schemaname,
        relname AS tablename,
        attname AS attname,
        stanullfrac AS null_frac,
        stawidth AS avg_width,
        stadistinct AS n_distinct,
        CASE
            WHEN stakind1 = 1 THEN stavalues1
            WHEN stakind2 = 1 THEN stavalues2
            WHEN stakind3 = 1 THEN stavalues3
            WHEN stakind4 = 1 THEN stavalues4
            WHEN stakind5 = 1 THEN stavalues5
        END AS most_common_vals,
        CASE
            WHEN stakind1 = 1 THEN stanumbers1
            WHEN stakind2 = 1 THEN stanumbers2
            WHEN stakind3 = 1 THEN stanumbers3
            WHEN stakind4 = 1 THEN stanumbers4
            WHEN stakind5 = 1 THEN stanumbers5
        END AS most_common_freqs,
        CASE
            WHEN stakind1 = 2 THEN stavalues1
            WHEN stakind2 = 2 THEN stavalues2
            WHEN stakind3 = 2 THEN stavalues3
            WHEN stakind4 = 2 THEN stavalues4
            WHEN stakind5 = 2 THEN stavalues5
        END AS histogram_bounds,
        CASE
            WHEN stakind1 = 3 THEN stanumbers1[1]
            WHEN stakind2 = 3 THEN stanumbers2[1]
            WHEN stakind3 = 3 THEN stanumbers3[1]
            WHEN stakind4 = 3 THEN stanumbers4[1]
            WHEN stakind5 = 3 THEN stanumbers5[1]
        END AS correlation,
        CASE
            WHEN stakind1 = 4 THEN stavalues1
            WHEN stakind2 = 4 THEN stavalues2
            WHEN stakind3 = 4 THEN stavalues3
            WHEN stakind4 = 4 THEN stavalues4
            WHEN stakind5 = 4 THEN stavalues5
        END AS most_common_elems,
        CASE
            WHEN stakind1 = 4 THEN stanumbers1
            WHEN stakind2 = 4 THEN stanumbers2
            WHEN stakind3 = 4 THEN stanumbers3
            WHEN stakind4 = 4 THEN stanumbers4
            WHEN stakind5 = 4 THEN stanumbers5
        END AS most_common_elem_freqs,
        CASE
            WHEN stakind1 = 5 THEN stanumbers1
            WHEN stakind2 = 5 THEN stanumbers2
            WHEN stakind3 = 5 THEN stanumbers3
            WHEN stakind4 = 5 THEN stanumbers4
            WHEN stakind5 = 5 THEN stanumbers5
        END AS elem_count_histogram
    FROM pg_statistic s JOIN pg_class c ON (c.oid = s.starelid)
         JOIN pg_attribute a ON (c.oid = attrelid AND attnum = s.staattnum)
         LEFT JOIN pg_namespace n ON (n.oid = c.relnamespace)
    WHERE NOT attisdropped;

-- minipg: function-in-FROM removed; call the set-returning function in a
-- subquery targetlist and expand the resulting row instead.

CREATE VIEW pg_locks AS
    SELECT L.* FROM (SELECT (pg_lock_status()).* ) AS L;

CREATE VIEW pg_prepared_xacts AS
    SELECT P.transaction, P.gid, P.prepared,
           'postgres'::name AS owner, D.datname AS database
    FROM (SELECT (pg_prepared_xact()).* ) AS P
         LEFT JOIN pg_database D ON P.dbid = D.oid;

CREATE VIEW pg_settings AS
    SELECT * FROM (SELECT (pg_show_all_settings()).* ) AS A;


CREATE VIEW pg_file_settings AS
   SELECT * FROM (SELECT (pg_show_all_file_settings()).* ) AS A;


CREATE VIEW pg_config AS
    SELECT * FROM (SELECT (pg_config()).* ) AS C;


CREATE VIEW pg_shmem_allocations AS
    SELECT * FROM (SELECT (pg_get_shmem_allocations()).* ) AS S;


CREATE VIEW pg_backend_memory_contexts AS
    SELECT * FROM (SELECT (pg_get_backend_memory_contexts()).* ) AS B;


-- Statistics views

CREATE VIEW pg_stat_activity AS
    SELECT
            S.datid AS datid,
            D.datname AS datname,
            S.pid,
            S.leader_pid,
            S.usesysid,
            'postgres'::name AS usename,
            S.application_name,
            S.client_addr,
            S.client_hostname,
            S.client_port,
            S.backend_start,
            S.xact_start,
            S.query_start,
            S.state_change,
            S.wait_event_type,
            S.wait_event,
            S.state,
            S.backend_xid,
            s.backend_xmin,
            S.query_id,
            S.query,
            S.backend_type
    FROM (SELECT (pg_stat_get_activity(NULL)).* ) AS S
        LEFT JOIN pg_database AS D ON (S.datid = D.oid);

CREATE VIEW pg_stat_progress_analyze AS
    SELECT
        S.pid AS pid, S.datid AS datid, D.datname AS datname,
        CAST(S.relid AS oid) AS relid,
        CASE S.param1 WHEN 0 THEN 'initializing'
                      WHEN 1 THEN 'acquiring sample rows'
                      WHEN 2 THEN 'acquiring inherited sample rows'
                      WHEN 3 THEN 'computing statistics'
                      WHEN 4 THEN 'computing extended statistics'
                      WHEN 5 THEN 'finalizing analyze'
                      END AS phase,
        S.param2 AS sample_blks_total,
        S.param3 AS sample_blks_scanned,
        S.param4 AS ext_stats_total,
        S.param5 AS ext_stats_computed,
        S.param6 AS child_tables_total,
        S.param7 AS child_tables_done,
        CAST(S.param8 AS oid) AS current_child_table_relid
    FROM (SELECT (pg_stat_get_progress_info('ANALYZE')).* ) AS S
        LEFT JOIN pg_database D ON S.datid = D.oid;

CREATE VIEW pg_stat_progress_vacuum AS
    SELECT
        S.pid AS pid, S.datid AS datid, D.datname AS datname,
        S.relid AS relid,
        CASE S.param1 WHEN 0 THEN 'initializing'
                      WHEN 1 THEN 'scanning heap'
                      WHEN 2 THEN 'vacuuming indexes'
                      WHEN 3 THEN 'vacuuming heap'
                      WHEN 4 THEN 'cleaning up indexes'
                      WHEN 5 THEN 'truncating heap'
                      WHEN 6 THEN 'performing final cleanup'
                      END AS phase,
        S.param2 AS heap_blks_total, S.param3 AS heap_blks_scanned,
        S.param4 AS heap_blks_vacuumed, S.param5 AS index_vacuum_count,
        S.param6 AS max_dead_tuples, S.param7 AS num_dead_tuples
    FROM (SELECT (pg_stat_get_progress_info('VACUUM')).* ) AS S
        LEFT JOIN pg_database D ON S.datid = D.oid;

CREATE VIEW pg_stat_progress_cluster AS
    SELECT
        S.pid AS pid,
        S.datid AS datid,
        D.datname AS datname,
        S.relid AS relid,
        CASE S.param1 WHEN 1 THEN 'CLUSTER'
                      WHEN 2 THEN 'VACUUM FULL'
                      END AS command,
        CASE S.param2 WHEN 0 THEN 'initializing'
                      WHEN 1 THEN 'seq scanning heap'
                      WHEN 2 THEN 'index scanning heap'
                      WHEN 3 THEN 'sorting tuples'
                      WHEN 4 THEN 'writing new heap'
                      WHEN 5 THEN 'swapping relation files'
                      WHEN 6 THEN 'rebuilding index'
                      WHEN 7 THEN 'performing final cleanup'
                      END AS phase,
        CAST(S.param3 AS oid) AS cluster_index_relid,
        S.param4 AS heap_tuples_scanned,
        S.param5 AS heap_tuples_written,
        S.param6 AS heap_blks_total,
        S.param7 AS heap_blks_scanned,
        S.param8 AS index_rebuild_count
    FROM (SELECT (pg_stat_get_progress_info('CLUSTER')).* ) AS S
        LEFT JOIN pg_database D ON S.datid = D.oid;

CREATE VIEW pg_stat_progress_create_index AS
    SELECT
        S.pid AS pid, S.datid AS datid, D.datname AS datname,
        S.relid AS relid,
        CAST(S.param7 AS oid) AS index_relid,
        CASE S.param1 WHEN 1 THEN 'CREATE INDEX'
                      WHEN 2 THEN 'CREATE INDEX CONCURRENTLY'
                      END AS command,
        CASE S.param10 WHEN 0 THEN 'initializing'
                       WHEN 1 THEN 'waiting for writers before build'
                       WHEN 2 THEN 'building index'
                       WHEN 3 THEN 'waiting for writers before validation'
                       WHEN 4 THEN 'index validation: scanning index'
                       WHEN 5 THEN 'index validation: sorting tuples'
                       WHEN 6 THEN 'index validation: scanning table'
                       WHEN 7 THEN 'waiting for old snapshots'
                       WHEN 8 THEN 'waiting for readers before marking dead'
                       WHEN 9 THEN 'waiting for readers before dropping'
                       END as phase,
        S.param4 AS lockers_total,
        S.param5 AS lockers_done,
        S.param6 AS current_locker_pid,
        S.param16 AS blocks_total,
        S.param17 AS blocks_done,
        S.param12 AS tuples_total,
        S.param13 AS tuples_done
    FROM (SELECT (pg_stat_get_progress_info('CREATE INDEX')).* ) AS S
        LEFT JOIN pg_database D ON S.datid = D.oid;

