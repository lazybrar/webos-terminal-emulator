#!/bin/bash
# make-sysroot32.sh [dir]  -- 32-bit ARM glibc 2.31 headers/crt/libs (Debian bullseye armel, same glibc as the TV) for building
# GPU programs that use the normal C library (terminal/wlterm2.c -DWLTERM_GL). Used only at build time; the program runs against the
# TV's own libc. Packages come from archive.debian.org and are checked against pinned SHA-256 sums.
set -e
d=${1:-sysroot/debian}; B=https://archive.debian.org/debian/pool/main
pk=(
 "g/glibc/libc6_2.31-13+deb11u11_armel.deb 344eab6e773a6b09e1501acc11d730ba9c035d074b716db72c37f1e5c0512bb3"
 "g/glibc/libc6-dev_2.31-13+deb11u11_armel.deb 329e9ca7208683ca69439a3aad0d5c60cedad1db173c726591f9740655cf8814"
 "g/gcc-10/libgcc-10-dev_10.2.1-6_armel.deb 9445a71c8f566147d1433ed47e233a1976ae886005fc5bc52664707e3e262115"
 "g/gcc-10/libgcc-s1_10.2.1-6_armel.deb 4f022b704c0b8cbf28ed44250a0819373e2374e814b85807d7c918d22e6e1b03"
 "l/linux/linux-libc-dev_5.10.223-1_armel.deb b22a053b3b3143ea28aa0b6c690d55ce61cbecd5f9f65ea59cbe3ec04c4b8488")
mkdir -p "$d"; d=$(cd "$d" && pwd)
for e in "${pk[@]}"; do
  set -- $e; w=$(mktemp -d); trap 'rm -rf "$w"' EXIT     # each download gets its own empty directory
  curl -sSfL -o "$w/p.deb" "$B/$1"; echo "$2  $w/p.deb" | sha256sum -c --quiet - || { echo "checksum mismatch: $1" >&2; exit 1; }
  ( cd "$w" && ar x p.deb && tar -xf data.tar.* ); cp -a "$w/usr" "$w/lib" "$d/" 2>/dev/null || cp -a "$w/usr" "$d/"
  rm -rf "$w"; echo "ok $(basename "$1")"
done
echo "sysroot: $d"
