#!/bin/sh
# Build org.tvwl.terminal ipk (ar archive of debian-binary, control.tar.gz, data.tar.gz) like ares-package does.
set -e
here=$(cd "$(dirname "$0")" && pwd); id=org.tvwl.terminal; out=${1:-$here/$id.ipk}
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT
mkdir -p "$w/data/usr/palm/applications/$id" "$w/control"
cp "$here/$id"/appinfo.json "$here/$id"/index.html "$here/$id"/icon80.png "$here/$id"/icon130.png "$w/data/usr/palm/applications/$id/"
printf 'Package: %s\nVersion: 1.0.1\nSection: misc\nPriority: optional\nArchitecture: all\nInstalled-Size: 40\nMaintainer: tvwl <noreply@example.invalid>\nDescription: wlterm2 launcher\nWebos-Package-Format-Version: 2\nWebos-Packager-Info: {"app":"%s"}\n' "$id" "$id" > "$w/control/control"
printf '2.0\n' > "$w/debian-binary"
tar --owner=0 --group=0 -C "$w/control" -czf "$w/control.tar.gz" .
tar --owner=0 --group=0 -C "$w/data" -czf "$w/data.tar.gz" .
# ar container written by hand: webOS wants names without the trailing "/" that GNU ar adds
printf '!<arch>\n' > "$out"
for f in debian-binary control.tar.gz data.tar.gz; do
  sz=$(stat -c %s "$w/$f")
  printf '%-16s%-12s%-6s%-6s%-8s%-10s`\n' "$f" 0 0 0 100644 "$sz" >> "$out"
  cat "$w/$f" >> "$out"
  [ $((sz % 2)) -eq 1 ] && printf '\n' >> "$out"
done
echo "$out"
