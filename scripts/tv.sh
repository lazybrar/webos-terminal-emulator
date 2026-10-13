#!/bin/bash
# tv.sh 'command'  -- run one command on the TV over telnet (port 23), print only its output.
# env: TV_IP (required), T = seconds to wait for output (default 8)
: "${TV_IP:?set TV_IP to the TV address}"
f=$(mktemp); trap 'rm -f "$f"' EXIT
( exec 3<>/dev/tcp/"$TV_IP"/23; sleep 0.7
  printf 'echo __BEG""IN__; %s\necho __EN""D__\n' "$1" >&3
  timeout "${T:-8}" sed -u '/^__END__/q' <&3 > "$f" )
strings -n1 "$f" | tr -d '\r' | sed -n '/^__BEGIN__$/,/^__END__$/p' | sed '1d;$d'
