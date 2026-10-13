#!/bin/bash
# pull-sysroot.sh [dir]  -- copy the TV's own GPU/Wayland libraries here so the 32-bit GPU examples can link against them
here=$(cd "$(dirname "$0")" && pwd); d=${1:-sysroot}; mkdir -p "$d"
for f in /usr/lib/libEGL.so.1 /usr/lib/libGLESv2.so.2 /usr/lib/libwayland-egl.so.1 /usr/lib/libwayland-client.so.0 \
         /usr/lib/libwayland-webos-client.so.1 /lib/libc.so.6; do "$here/pull.sh" "$f" "$d/$(basename "$f")"; done
ls -l "$d"
