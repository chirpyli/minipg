--
-- num_nulls()
--

SELECT num_nonnulls(NULL);
SELECT num_nonnulls('1');
SELECT num_nonnulls(NULL::text);
SELECT num_nonnulls(NULL::text, NULL::int);
SELECT num_nonnulls(1, 2, NULL::text, NULL::point, '', int8 '9', 1.0 / NULL);

SELECT num_nulls(NULL);
SELECT num_nulls('1');
SELECT num_nulls(NULL::text);
SELECT num_nulls(NULL::text, NULL::int);
SELECT num_nulls(1, 2, NULL::text, NULL::point, '', int8 '9', 1.0 / NULL);

-- should fail, one or more arguments is required
SELECT num_nonnulls();
SELECT num_nulls();

--
-- pg_log_backend_memory_contexts()
--
-- Memory contexts are logged and they are not returned to the function.
-- Furthermore, their contents can vary depending on the timing. However,
-- we can at least verify that the code doesn't fail.
--
SELECT * FROM pg_log_backend_memory_contexts(pg_backend_pid());

