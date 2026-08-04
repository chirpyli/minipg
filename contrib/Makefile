# contrib/Makefile

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		amcheck		\
		bloom		\
		pageinspect	\
		pg_buffercache	\
		pg_freespacemap \
		pg_stat_statements \
		pg_surgery	\
		pgrowlocks	\
		pgstattuple	\
		pg_visibility	\
		spi		\
		test_decoding

# 注：test_decoding 为逻辑复制（replication）的调试/输出插件，被
# src/test/recovery、src/test/subscription、src/bin/pg_basebackup 等测试依赖，
# 属 replication 子系统。在 minipg 裁剪路线中将在"阶段 8 裁 replication"时
# 连同上述测试一并删除，当前（replication 仍保留）不能删，否则 check-world 失败。

# 以下扩展已按 minipg 内核学习裁剪方案（方案 A）删除：
#   adminpack auth_delay auto_explain btree_gin btree_gist citext cube
#   dblink dict_int dict_xsyn earthdistance file_fdw fuzzystrmatch hstore
#   intagg intarray isn lo ltree oid2name old_snapshot passwordcheck pg_trgm
#   pgcrypto postgres_fdw seg sepgsql sslinfo tablefunc tcn test_decoding
#   tsm_system_rows tsm_system_time unaccent vacuumlo xml2 uuid-ossp
#   bool_plperl hstore_plperl jsonb_plperl hstore_plpython jsonb_plpython
#   ltree_plpython start-scripts
# 保留的均为内核观察类（pageinspect/pg_buffercache/pg_freespacemap/
# pg_visibility/pgstattuple/pg_stat_statements/pg_surgery/
# pgrowlocks/amcheck）及示例型（bloom 自定义 AM、spi 服务端接口）。

$(recurse)
$(recurse_always)
