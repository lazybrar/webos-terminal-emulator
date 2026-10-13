# webos-terminal-emulator

Native Wayland programs for a **rooted LG webOS TV**: a TrueType terminal emulator, a GPU (PowerVR / GLES2) example,
a launcher icon, and notes on how all of it works.

| Part | What it is |
|---|---|
| [`terminal/wlterm2.c`](terminal/wlterm2.c) | Terminal emulator: Fira Code (any TTF), truecolor, UTF-8, scrollback, alternate screen, `xterm-256color`. 64-bit static. |
| [`gpu/wlgl.c`](gpu/wlgl.c) | GLES2 triangle on the TV's own PowerVR GPU through EGL. 32-bit, links the TV's libs. |
| [`examples/`](examples) | `wlglobals` (list compositor globals), `shm-window` (smallest possible window), no libc. |
| [`app/`](app) | Launcher app (icon on the home screen) built as an `.ipk`. |
| [`wlterm.conf.example`](wlterm.conf.example) | Optional settings: shell, home, start dir, PATH, font, size, env. |
| [`overlay/`](overlay) | Boot hook that makes `/usr`, `/lib`, `/etc` writable and persistent on top of the read-only squashfs. |
| [`scripts/`](scripts) | Telnet/HTTP helpers: run commands, copy files, inject keystrokes, take screenshots. |

Docs: **[running the terminal](docs/terminal.md)** · **[getting the fish shell](docs/fish.md)** · **[writing Wayland apps for webOS](docs/wayland-apps.md)** · **[writing GPU apps](docs/gpu-apps.md)** · **[system notes (overlay, Alpine userland, test loop)](docs/system.md)**

## Facts that shape everything

* The kernel is **64-bit** (aarch64) but the userland is **32-bit ARM, soft-float ABI, glibc 2.31** (`/lib/ld-linux.so.3`).
* 64-bit **static** binaries run fine (no loader needed). They cannot use the GPU: the PowerVR user-space driver is 32-bit only.
* GPU apps must be **32-bit ARM** programs linked against the TV's own `libEGL` / `libGLESv2` / `libwayland-*`.
* The compositor speaks `wl_shm`, `wl_shell`, `wl_drm`, `zwp_linux_dmabuf_v1`, `wl_seat` and LG's `wl_webos_shell`. There is **no `xdg_shell`**.
* A window only stays up if it registers an `appId` through `wl_webos_shell` (see [gpu-apps.md](docs/gpu-apps.md)). A bare `wl_shell` surface is dropped after about a second.

## Quick start

Requirements: a rooted webOS TV with telnet (port 23) and the Homebrew Channel; on the PC `aarch64-linux-gnu-gcc`, `stb` (`stb/stb_truetype.h`), `clang` + `lld`; optional `resvg`, `ffmpeg`.

```sh
export TV_IP=<address of your TV>
make                                   # build/wlterm2, build/wlglobals, build/shm-window
scripts/deploy.sh build/wlterm2 /media/developer/bin/wlterm2
scripts/deploy.sh scripts/wlterm-launch /media/developer/bin/wlterm-launch
# put Fira Code on the TV (see docs/terminal.md), then:
scripts/tv.sh 'XDG_RUNTIME_DIR=/tmp/xdg setsid /media/developer/bin/wlterm2 </dev/null >/tmp/wlterm2.log 2>&1 &'
```

## Status (what was tested on a real TV)

Tested on webOS 7.5.8 (LG, Realtek SoC, PowerVR GPU): window creation and `appId` recipe, the GLES2 example (about 60 fps),
terminal rendering with Fira Code, truecolor, 256-color, bold, underline, strike, cursor, key input, the persistent overlay
across a reboot, the launcher app and its icon, the screenshot and keystroke-injection loop.
Implemented but **not yet verified on hardware**: scrollback (Shift+PgUp/PgDn), alternate screen apps (vim, tmux, htop),
scroll regions, DEC line drawing.
Known gaps: no ligatures (needs OpenType shaping), no double-width (CJK) characters, no clipboard, no mouse, no italic face,
box-drawing glyphs come from the font and do not always join cell to cell, SDL2 apps untested.

## Safety

This modifies a TV's filesystem as root. Never write to the kernel, rootfs or TV service partitions
(see <https://rootmy.tv/warning>); everything here lives on writable overlays and `/media/developer`. Use at your own risk.

License: GPL-2.0-or-later. Fira Code (OFL) and `stb_truetype` (public domain) are **not** bundled.
