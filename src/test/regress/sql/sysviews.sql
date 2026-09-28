
--
-- Test assorted system set-returning functions
--
-- minipg: the system views that used to wrap these set-returning functions
-- have been cropped, so we call the functions directly.  The output of most
-- of these functions is very environment-dependent, so our ability to test
-- with fixed expected output is pretty limited; but even a trivial query
-- against them will exercise the normal code path through the SRF.

-- The entire output of pg_get_backend_memory_contexts is not stable,
-- we test only the existance and basic condition of TopMemoryContext.
select name, ident, parent, level, total_bytes >= free_bytes
  from (select (pg_get_backend_memory_contexts()).*) as b where level = 0;

-- At introduction, pg_config had 23 entries; it may grow
select DISTINCT true as ok from (select (pg_config()).*) as c;

select DISTINCT true as ok from (select (pg_show_all_file_settings()).*) as fs;

-- There will surely be at least one active lock
select DISTINCT true as ok from (select (pg_lock_status()).*) as l;

-- This is to record the prevailing planner enable_foo settings during
-- a regression test run.
select name, setting
  from (select (pg_show_all_settings()).*) as s
  where left(name, 6) = 'enable';
