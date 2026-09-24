#!/bin/bash
# probe current bsh behavior for exit-status handling
B="/tmp/bsh-host"
echo "--- pipeline ---"
$B -c 'true | false'; echo "true|false => $?"
$B -c 'false | true'; echo "false|true => $?"
$B -c 'echo hi | grep zzz'; echo "echo|grep-no-match => $?"
echo "--- single external ---"
for i in 1 2 3 4 5; do
  $B -c '/bin/false'; echo "/bin/false run $i => $?"
done
$B -c '/bin/true'; echo "/bin/true => $?"
echo "--- single builtin ---"
$B -c 'false'; echo "false => $?"
$B -c 'true'; echo "true => $?"
