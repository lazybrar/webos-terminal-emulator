#!/bin/bash
# pull-sysroot.sh [dir]  -- copy the TV's own GPU/Wayland libraries here (link-time only) so the 32-bit GPU programs can link.
# The TV uploads them with curl to a short-lived receiver on this PC (bound to the LAN address, accepts PUT of exactly these names).
# env: TV_IP (required), PC_IP (default: source address used to reach the TV), PORT (default 8089)
set -e
: "${TV_IP:?set TV_IP to the TV address}"
here=$(cd "$(dirname "$0")" && pwd); d=${1:-sysroot}; mkdir -p "$d"
PC_IP=${PC_IP:-$(ip -4 route get "$TV_IP" | sed -n 's/.* src \([0-9.]*\).*/\1/p')}; PORT=${PORT:-8089}
files="libc.so.6 libEGL.so.1 libGLESv2.so.2 libwayland-egl.so.1 libwayland-client.so.0 libwayland-webos-client.so.1"
python3 -I - "$d" "$PC_IP" "$PORT" $files <<'PY' &
import http.server, sys, os
out, ip, port, allowed = sys.argv[1], sys.argv[2], int(sys.argv[3]), set(sys.argv[4:])
class H(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        n = self.path.lstrip("/")
        if n not in allowed: self.send_response(403); self.end_headers(); return
        data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        open(os.path.join(out, n), "wb").write(data); self.send_response(200); self.end_headers()
    def log_message(self, *a): pass
http.server.HTTPServer((ip, port), H).serve_forever()
PY
srv=$!; trap 'kill $srv 2>/dev/null' EXIT; sleep 1
T=90 "$here/tv.sh" "for f in $files; do p=/usr/lib/\$f; [ -e \$p ] || p=/lib/\$f; curl -sS -m 60 -T \$p http://$PC_IP:$PORT/\$f || echo FAILED \$f; done"
ls -l "$d"
