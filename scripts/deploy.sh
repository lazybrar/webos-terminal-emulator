#!/bin/bash
# deploy.sh local/file /remote/path  -- fast PC -> TV copy: the TV wgets the (gzipped) file from a
# short-lived HTTP server bound to this PC's LAN address that serves one temp dir containing only that file.
# env: TV_IP (required), PC_IP (default: the source address used to reach the TV), PORT (default 8088)
set -e
: "${TV_IP:?set TV_IP to the TV address}"
here=$(cd "$(dirname "$0")" && pwd)
PC_IP=${PC_IP:-$(ip -4 route get "$TV_IP" | sed -n 's/.* src \([0-9.]*\).*/\1/p')}; PORT=${PORT:-8088}
d=$(mktemp -d); gzip -9 -c "$1" > "$d/f.gz"
python3 -m http.server "$PORT" --bind "$PC_IP" --directory "$d" >/dev/null 2>&1 & srv=$!
trap 'kill $srv 2>/dev/null; rm -rf "$d"' EXIT; sleep 1
T=60 "$here/tv.sh" "wget -q -O /tmp/f.gz http://$PC_IP:$PORT/f.gz && gzip -dc /tmp/f.gz > '$2.new' && chmod +x '$2.new' && mv '$2.new' '$2' && rm /tmp/f.gz && ls -l '$2'"
