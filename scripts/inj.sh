#!/bin/bash
# inj.sh 'text'  -- type into the running wlterm2 through its control fifo (escapes: \n \r \t \e \\ \xHH)
here=$(cd "$(dirname "$0")" && pwd)
"$here/tv.sh" "echo 'in $1' > /tmp/wlterm2.ctl" >/dev/null
