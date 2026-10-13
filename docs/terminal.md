# Running the terminal emulator (`wlterm2` / `wlterm-gl`)

One terminal, two renderers built from the same source (`terminal/wlterm2.c`):

| | `wlterm2` (CPU) | `wlterm-gl` (GPU) |
|---|---|---|
| Drawing | `stb_truetype` rasterises into a `wl_shm` buffer | glyph atlas texture + one GLES2 quad per cell on the TV's PowerVR |
| Build | **64-bit static** aarch64: no loader, no libraries | **32-bit ARM**, dynamic, links the TV's EGL/GLES2/libwayland (the PowerVR driver is 32-bit only) |
| Make target | `make build/wlterm2` | `make gl-term` (see [GPU build](#gpu-build)) |

`wlterm-launch` chooses with the `renderer` setting: `auto` (default) uses the GPU build if installed and falls back to the CPU one if
it does not start, `gl` / `cpu` force one. The terminal logic (escape sequences, scrollback, keyboard, settings) is shared; only window
creation, input plumbing and drawing differ (`terminal/front_gl.inc`).

**What the GPU buys (measured on the TV, `seq 1 200000` flooding the screen, same shell, same drain logic):** the GPU build takes a steady
~1.47 s using ~1.0 s of CPU; the CPU build takes 1.4-2.5 s using 1.1-2.3 s of CPU (the shell and pty set the floor at ~1.4 s). So the
benefit is mainly **lower and steadier CPU load** and headroom for the programs you run, not a big wall-clock gain; idle redraws are cheap in both.
Before the PTY-drain change (read the shell's output for a few ms before redrawing) a flood took 3-6 s on either renderer.

## Build

```sh
sudo pacman -S aarch64-linux-gnu-gcc stb      # or your distro's equivalents
make build/wlterm2                           # aarch64-linux-gnu-gcc -static ... -idirafter /usr/include
```

`stb_truetype.h` is the only dependency (header only). The `-idirafter` makes the cross compiler see the host copy.

## GPU build

```sh
export TV_IP=<address of your TV>
scripts/make-sysroot32.sh          # 32-bit glibc 2.31 headers/crt/libs from Debian bullseye armel (checksum-pinned), build time only
scripts/pull-sysroot.sh            # the TV's own libEGL / libGLESv2 / libwayland-* for linking
make gl-term                       # build/wlterm-gl; `make ipk` then bundles it
```

The binary needs only glibc symbols up to 2.29 (the TV has 2.31) and uses the TV's libraries at run time. The TV's libwayland is old and lacks
`wl_proxy_marshal_flags`, so `front_gl.inc` calls the older marshalling functions directly instead of the inline wrappers in current headers.

## Install on the TV

Install the ipk (see the README): it contains `wlterm2` and `bin/wlterm-launch` under
`/media/developer/apps/usr/palm/applications/org.tvwl.terminal/`, and the first launch downloads Fira Code into
`/media/developer/fonts/`. For development, copy the binary yourself:

```sh
export TV_IP=<address of your TV>
scripts/deploy.sh build/wlterm2 /media/developer/bin/wlterm2
scripts/deploy.sh scripts/wlterm-launch /media/developer/bin/wlterm-launch     # looks for wlterm2 next to itself
```

`deploy.sh` starts a short-lived HTTP server on your PC (bound to the LAN address, serving one temp directory that holds only the
file) and has the TV `wget` it, then stops the server. Telnet itself moves only 1-2 KB/s, so use it only for commands.

## Run

```sh
scripts/tv.sh 'XDG_RUNTIME_DIR=/tmp/xdg setsid /media/developer/bin/wlterm2 </dev/null >/tmp/wlterm2.log 2>&1 &'
```

Run it **detached** (`setsid`, no inherited tty), otherwise it dies with your telnet session.
Close it with `exit` in the shell, or `echo quit > /tmp/wlterm2.ctl`.

## Settings

Optional file `/media/developer/wlterm.conf` (override with `-c file` or `WLTERM_CONF`); see [`wlterm.conf.example`](../wlterm.conf.example).
`key = value` lines, `#` comments: `shell`, `home`, `cwd`, `path` (appended to `PATH`), `size`, `font`, `bold`, `fallback` (repeatable),
`env = NAME=VALUE` (repeatable). Command-line flags win over the file, the file wins over the defaults. The default `HOME` (and start
directory) is `/media/developer/home`, created on first start; the shell is Alpine's fish if installed, else `/bin/sh`.

Options (both builds): `-c conf` `-H home` `-d start-dir` `-p extra-PATH` `-f regular.ttf` `-b bold.ttf` `-x fallback.ttf` (repeatable, e.g. a Nerd Font for icons) `-s <pixel size>` (default 30) `-e <shell>`.
The launcher forwards its arguments, e.g. `wlterm-launch -e /bin/sh -d /tmp`.
Environment for the shell: `TERM=xterm-256color`,
`COLORTERM=truecolor`, `PATH` extended with `/usr/alpine/...` (see [system.md](system.md)).

Keyboard input comes from the compositor (`wl_keyboard`, US layout): a USB/Bluetooth keyboard works, the remote only sends its own keys.
Shift+PgUp/PgDn scrolls the 5000-line scrollback; arrows, Home/End, PgUp/PgDn, Ins/Del, F1-F12 and Ctrl/Alt/Shift modifiers are sent
with xterm sequences; keys auto-repeat.

## Home-screen icon

`make ipk` builds it (and `scripts/deploy.sh` + `luna-send ... appInstallService/dev/install` installs it, see the README).

The app is a tiny web app. Its page calls `luna://org.webosbrew.hbchannel.service/exec` (root, provided by the Homebrew Channel) to run
the bundled `bin/wlterm-launch` (found from the page's own URL), then closes itself. A *native* app type would run inside webOS's sandbox jail and lose system access, which is why the
launcher is a web app. Clicking the icon again while a terminal is running **restarts** it (`wlterm-launch` closes the old one and starts a new one; the old window is hidden behind the launcher card and cannot be re-shown). The page launches on load and on every `webOSRelaunch`, and `appinfo.json` sets `handlesRelaunch`; without that a second click only flashes the card and drops to Home. Launch it from the icon, or `luna-send -n 1 luna://com.webos.service.applicationmanager/launch '{"id":"org.tvwl.terminal"}'`.
The `.ipk` container must use `ar` member names **without** the trailing `/` (GNU `ar` adds it and the installer then reports
"IPK Extraction Failure"), so `app/build-ipk.sh` writes the archive itself.

## Testing the terminal (inject keys, read the screen, take screenshots)

`wlterm2` creates a fifo `/tmp/wlterm2.ctl` (override with `WLTERM_CTL`). One command per line:

| Command | Effect |
|---|---|
| `in <text>` | type text into the shell (`\n \r \t \e \\ \xHH` escapes) |
| `text <file>` | dump the visible screen as UTF-8 text |
| `shot <file>` | dump the framebuffer as a binary PPM (P6) |
| `quit` | exit |

```sh
scripts/inj.sh 'ls /usr\n'      # type
scripts/shot.sh shot.png        # PPM -> gzip on the TV -> pulled over telnet -> PNG (needs ffmpeg)
```

## Limits

No ligatures, no double-width characters, no clipboard or mouse, one fixed 1920x1080 fullscreen window.
