
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

#########################################
# Run pg_test_timing and log its output for data collection purposes.

my $cmd = [ 'pg_test_timing', '--duration' => '1' ];
my ($stdout, $stderr);
print("# Running: " . join(" ", @{$cmd}) . "\n");
my $result = IPC::Run::run $cmd, '>' => \$stdout, '2>' => \$stderr;
is($result, 1, "pg_test_timing: exit code 0");

note "pg_test_timing output:\n$stdout\n";
note "pg_test_timing stderr:\n$stderr\n" if $stderr;

# Intentionally fail so CI preserves artifacts for log inspection
fail("intentional failure to preserve CI artifacts");

done_testing();
