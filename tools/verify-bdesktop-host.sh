#!/bin/bash
set -e
bash tools/build-bdesktop-host.sh 2>/tmp/bdesk-host/warn.log || exit 1
ls -l /tmp/bdesk-host/bdesktop
/tmp/bdesk-host/bdesktop --version
echo "HOST-BUILD-OK"
