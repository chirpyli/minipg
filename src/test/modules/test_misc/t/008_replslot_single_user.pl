# Copyright (c) 2025, PostgreSQL Global Development Group

# Test manipulations of replication slots with the single-user mode.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Skip the tests on Windows, as single-user mode would fail on permission
# failure with privileged accounts.
if ($windows_os)
{
	plan skip_all => 'this test is not supported by this platform';
}

# Run set of queries in single-user mode.
sub test_single_mode
{
	my ($node, $queries, $testname) = @_;

	my $result = run_log(
		[
			'postgres', '--single', '-F',
			'-c' => 'exit_on_error=true',
			'-D' => $node->data_dir,
			'postgres'
		],
		'<' => \$queries);

	ok($result, $testname);
}

my $slot_physical = 'slot_physical';

# Initialize a node
my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(allows_streaming => "replica");
$node->start;

# Define initial table
$node->safe_psql('postgres', "CREATE TABLE foo (id int)");

$node->stop;

test_single_mode(
	$node,
	"SELECT pg_create_physical_replication_slot('$slot_physical', true)",
	"physical slot creation");
test_single_mode(
	$node,
	"SELECT pg_create_physical_replication_slot('slot_tmp', true, true)",
	"temporary physical slot creation");

test_single_mode(
	$node,
	"SELECT pg_replication_slot_advance('$slot_physical', pg_current_wal_lsn())",
	"physical slot advance");

test_single_mode(
	$node,
	"SELECT pg_copy_physical_replication_slot('$slot_physical', 'slot_phy_copy')",
	"physical slot copy");

test_single_mode(
	$node,
	"SELECT pg_drop_replication_slot('$slot_physical')",
	"physical slot drop");

done_testing();
