/* src/test/modules/test_extensions/test_ext8--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext8" to load this file. \quit

-- create some random data type
create domain posint as int check (value > 0);

-- use it in regular tables and functions
-- 注：minipg 已移除临时表功能，原脚本中 create temp table / pg_temp. 函数已删除

create table ext8_table1 (f1 posint);

create function ext8_even (posint) returns bool as
  'select ($1 % 2) = 0' language sql;
