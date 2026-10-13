#!/bin/bash
# shot.sh out.png  -- screenshot of wlterm2's framebuffer (needs ffmpeg on the PC)
here=$(cd "$(dirname "$0")" && pwd); t=$(mktemp)
"$here/tv.sh" 'echo "shot /tmp/s.ppm" > /tmp/wlterm2.ctl; sleep 2; gzip -1 -c /tmp/s.ppm > /tmp/s.ppm.gz; rm /tmp/s.ppm' >/dev/null
"$here/pull.sh" /tmp/s.ppm.gz "$t" && gzip -dc "$t" | ffmpeg -loglevel error -y -f image2pipe -vcodec ppm -i - -vf scale=1280:-1 "$1"; rm -f "$t"
