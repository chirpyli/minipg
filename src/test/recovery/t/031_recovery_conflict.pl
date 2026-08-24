# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test that connections to a hot standby are correctly canceled when a
# recovery conflict is detected Also, test that statistics in
# pg_stat_database_conflicts are populated correctly

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => "disabled until after minor releases, due to instability";

# Set up nodes
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(has_archiving => 1);

$node_primary->append_conf(
	'postgresql.conf', qq[

log_temp_files = 0

# for deadlock test
max_prepared_transactions = 10

# wait some to test the wait paths as well, but not long for obvious reasons
max_standby_streaming_delay = 50ms

# Some of the recovery conflict logging code only gets exercised after
# deadlock_timeout. The test doesn't rely on that additional output, but it's
# nice to get some minimal coverage of that code.
log_recovery_conflict_waits = on
deadlock_timeout = 10ms
]);
$node_primary->start;

my $backup_name = 'my_backup';

$node_primary->backup($backup_name);
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_restoring => 1);

$node_standby->start;

my $test_db = "test_db";

# use a new database, to trigger database recovery conflict
$node_primary->safe_psql('postgres', "CREATE DATABASE $test_db");

# test schema / data
my $table1 = "test_recovery_conflict_table1";
my $table2 = "test_recovery_conflict_table2";
$node_primary->safe_psql(
	$test_db, qq[
CREATE TABLE ${table1}(a int, b int);
INSERT INTO $table1 SELECT i % 3, 0 FROM generate_series(1,20) i;
CREATE TABLE ${table2}(a int, b int);
]);
my $primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);


# a longrunning psql that we can use to trigger conflicts
my $psql_standby = $node_standby->background_psql($test_db,
	on_error_stop => 0);
my $expected_conflicts = 0;


# minipg: cursors removed.  The original "buffer pin conflict" test relied on a
# DECLARE CURSOR to pin a buffer on the standby; there is no SQL-level way to
# hold a buffer pin across statements anymore, so that conflict is dropped.
# The "snapshot conflict" test now holds a snapshot with a plain REPEATABLE
# READ transaction instead of a cursor.

# to check the log starting now for recovery conflict messages
my $log_location = -s $node_standby->logfile;

## RECOVERY CONFLICT 2: Snapshot conflict
my $sect = "snapshot conflict";
$expected_conflicts++;

$node_primary->safe_psql($test_db,
	qq[INSERT INTO $table1 SELECT i, 0 FROM generate_series(1,20) i]);
$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

# Open a REPEATABLE READ transaction and read the table on the standby,
# establishing a snapshot that will conflict with vacuum's pruning
my $res = $psql_standby->query_safe(qq[
        BEGIN ISOLATION LEVEL REPEATABLE READ;
        SELECT b FROM $table1;
        ]);
like($res, qr/^0$/m, "$sect: transaction with conflicting snapshot established");

# Do some HOT updates
$node_primary->safe_psql($test_db,
	qq[UPDATE $table1 SET a = a + 1 WHERE a > 2;]);

# VACUUM, pruning those dead tuples
$node_primary->safe_psql($test_db, qq[VACUUM $table1;]);

# Wait for attempted replay of PRUNE records
$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

check_conflict_log(
	"User query might have needed to see row versions that must be removed");
$psql_standby->reconnect_and_clear();
check_conflict_stat("snapshot");


## RECOVERY CONFLICT 3: Lock conflict
$sect = "lock conflict";
$expected_conflicts++;

# acquire lock to conflict with
$res = $psql_standby->query_safe(qq[
        BEGIN;
        LOCK TABLE $table1 IN ACCESS SHARE MODE;
        SELECT 1;
        ]);
like($res, qr/^1$/m, "$sect: conflicting lock acquired");

# DROP TABLE containing block which standby has in a pinned buffer
$node_primary->safe_psql($test_db, qq[DROP TABLE $table1;]);

$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

check_conflict_log("User was holding a relation lock for too long");
$psql_standby->reconnect_and_clear();
check_conflict_stat("lock");


# minipg: cursors removed.  The original "startup deadlock" conflict test
# (already disabled upstream due to instability) relied on a DECLARE CURSOR
# to hold a buffer pin while waiting for a lock; it is dropped.


# Check that expected number of conflicts show in pg_stat_database. Needs to
# be tested before database is dropped, for obvious reasons.
is( $node_standby->safe_psql(
		$test_db,
		qq[SELECT conflicts FROM pg_stat_database WHERE datname='$test_db';]),
	$expected_conflicts,
	qq[$expected_conflicts recovery conflicts shown in pg_stat_database]);


## RECOVERY CONFLICT 6: Database conflict
$sect = "database conflict";

$node_primary->safe_psql('postgres', qq[DROP DATABASE $test_db;]);

$primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_standby, 'replay', $primary_lsn);

check_conflict_log("User was connected to a database that must be dropped");


# explicitly shut down psql instances gracefully - to avoid hangs or worse on
# windows
$psql_standby->quit;

$node_standby->stop();
$node_primary->stop();


done_testing();

sub check_conflict_log
{
	my $message          = shift;
	my $old_log_location = $log_location;

	$log_location = $node_standby->wait_for_log(qr/$message/, $log_location);

	cmp_ok($log_location, '>', $old_log_location,
		"$sect: logfile contains terminated connection due to recovery conflict"
	);
}

sub check_conflict_stat
{
	# Stats can't easily be checked before 15, requires waiting for stats to
	# be reported to stats collector and then those messages need to be
	# processed. Dealt with here to reduce intra-branch difference in the
	# tests.
}
