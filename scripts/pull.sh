#!/bin/bash
# pull.sh /remote/file local/file  -- copy TV -> PC through the telnet session (base64, binary safe)
: "${TV_IP:?set TV_IP to the TV address}"
f=$(mktemp); trap 'rm -f "$f"' EXIT
( exec 3<>/dev/tcp/"$TV_IP"/23; sleep 0.7
  printf 'echo __BEG""IN__; base64 %s; echo __EN""D__\n' "$1" >&3
  timeout "${T:-120}" sed -u '/^__END__/q' <&3 > "$f" )
tr -d '\r' < "$f" | sed -n '/^__BEGIN__$/,/^__END__$/p' | sed '1d;$d' | base64 -d > "$2"
