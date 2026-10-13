#!/bin/sh
# Build org.tvwl.terminal ipk (ar archive of debian-binary, control.tar.gz, data.tar.gz) like ares-package does.
# Self-contained: bundles the launcher page, wlterm2 (build/wlterm2, run `make` first), bin/wlterm-launch and, if built (make gl-term),
# the GPU terminal wlterm-gl.
set -e
here=$(cd "$(dirname "$0")" && pwd); root=$(dirname "$here"); id=org.tvwl.terminal; out=${1:-$here/$id.ipk}
ver=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$here/$id/appinfo.json")
[ -n "$ver" ] || { echo "no version in appinfo.json" >&2; exit 1; }
[ -x "$root/build/wlterm2" ] || { echo "build/wlterm2 missing: run 'make build/wlterm2' first" >&2; exit 1; }
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT
app=$w/data/usr/palm/applications/$id
mkdir -p "$app/bin" "$w/control"
cp "$here/$id"/appinfo.json "$here/$id"/index.html "$here/$id"/icon80.png "$here/$id"/icon130.png "$app/"
install -m 755 "$root/build/wlterm2" "$app/bin/wlterm2"
install -m 755 "$root/scripts/wlterm-launch" "$app/bin/wlterm-launch"
[ -x "$root/build/wlterm-gl" ] && install -m 755 "$root/build/wlterm-gl" "$app/bin/wlterm-gl" || echo "note: build/wlterm-gl missing, ipk will be CPU-only (make gl-term)" >&2
size=$(du -sk "$w/data" | cut -f1)
printf 'Package: %s\nVersion: %s\nSection: misc\nPriority: optional\nArchitecture: all\nInstalled-Size: %s\nMaintainer: tvwl <noreply@example.invalid>\nDescription: Terminal emulator for rooted webOS TVs (wlterm2 + launcher)\nWebos-Package-Format-Version: 2\nWebos-Packager-Info: {"app":"%s"}\n' "$id" "$ver" "$size" "$id" > "$w/control/control"
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
