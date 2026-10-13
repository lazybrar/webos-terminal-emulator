# Writing Wayland apps for webOS

How to put your own native window on a rooted LG webOS TV. Everything here was worked out on webOS 7.5.8 (aarch64 kernel, 32-bit
userland) and is backed by working code in this repo: [`examples/shm-window.c`](../examples/shm-window.c) (smallest window),
[`terminal/wlterm2.c`](../terminal/wlterm2.c) (a real app with keyboard input), [`gpu/wlgl.c`](../gpu/wlgl.c) (GPU). For OpenGL ES see
[gpu-apps.md](gpu-apps.md).

## 1. Pick your path

| You want | Build as | Draws with | Notes |
|---|---|---|---|
| A tool, terminal, dashboard, anything CPU-drawn | **64-bit static** (`aarch64-linux-gnu-gcc -static`) | pixels in a `wl_shm` buffer | Easiest. No loader, no libraries. `wlterm2` does this at 1080p. |
| 3D, video-like animation, anything needing the GPU | **32-bit ARM, soft-float ABI**, dynamic, against the TV's libs | EGL + GLES2 on the PowerVR | The GPU driver is 32-bit only. See [gpu-apps.md](gpu-apps.md). |
| A UI toolkit app (SDL2, Qt) | 32-bit, the TV's own copies | whichever backend | Not tested here. Must still set the webOS `appId`. |

Why: the kernel is 64-bit, so static 64-bit binaries run with no loader; but every shared library on the TV (including the PowerVR
EGL/GLES driver) is 32-bit glibc 2.31 `armel`. A 64-bit process can't load them.

## 2. What the compositor gives you

Run `build/wlglobals` on the TV to list the globals. Expect `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_drm`,
`zwp_linux_dmabuf_v1`, `wl_seat`, `wl_shell`, `wl_output` and LG's `wl_webos_shell` (plus `wl_webos_*`/`wl_starfish_*` extras).

* **There is no `xdg_shell`.** Toolkits and apps that require it (GTK4, current Qt, foot, alacritty...) won't create a window. Either use
  the raw protocol as the examples do, or `wl_shell`/`wl_webos_shell` with a toolkit that supports them.
* The socket is `$XDG_RUNTIME_DIR/wayland-0`. On the TV that is **`/tmp/xdg/wayland-0`**; processes started from a login shell don't have
  `XDG_RUNTIME_DIR` set, so export `XDG_RUNTIME_DIR=/tmp/xdg`. The directory is `root:composit`, mode 0770, so run as root (or a member of `composit`).
* Output is 1920x1080. The compositor composes whatever size you attach; fullscreen is the normal case for a TV.

## 3. The recipe for a window that stays

A plain `wl_shell` toplevel is dropped by the TV's `surface-manager` after about a second. A window survives only if it announces an
**app id** through `wl_webos_shell`:

1. Bind `wl_compositor`, `wl_shm`, `wl_webos_shell`; create a `wl_surface`.
2. `wl_webos_shell.get_shell_surface(new_id, surface)` (request opcode 1).
3. On the new shell surface call `set_property(name, value)` (opcode 2, two strings) three times:
   `appId` = `<your.app.id>`, `_WEBOS_WINDOW_TYPE` = `_WEBOS_WINDOW_TYPE_CARD`, `instanceId` = `<your.app.id>`.
4. `set_state(3)` (opcode 1) = fullscreen.
5. Attach a buffer, damage, commit.

Confirm acceptance in the log: `grep NL_VSC /tmp/var/log/messages | tail`, which prints
`{"app_id":"<your.app.id>","visible":true,"window_type":"_WEBOS_WINDOW_TYPE_CARD"}` once it's shown (and `visible:false` when
something else takes the screen). The opcodes come from the message order in `libwayland-webos-client.so`; the property names were found
by experiment, so other window types (overlay, popup) are unexplored.

`wlterm2` treats event 2 on the webOS shell surface as "quit" (assumed to be the close request; not observed in practice, so treat as unverified).

## 4. Drawing with `wl_shm`

1. `memfd_create`, `ftruncate(w*h*4)`, `mmap` shared: this is your framebuffer. Draw ARGB8888 / XRGB pixels straight into it.
2. `wl_shm.create_pool(new_id, fd, size)` (send the fd as `SCM_RIGHTS` ancillary data), then
   `wl_shm_pool.create_buffer(new_id, offset, w, h, stride, format)` (format 0 = ARGB8888).
3. `wl_surface.attach(buffer, 0, 0)`, `damage(x, y, w, h)`, `commit()`.
4. To update, change the pixels, then `damage` + `commit` again on the same buffer. `wlterm2` redraws only dirty rows and damages the whole
   surface; with one shared buffer there is no tearing worth worrying about for a terminal. For animation use two buffers and
   `wl_buffer.release` events, or `wl_surface.frame` callbacks for pacing.

[`examples/shm-window.c`](../examples/shm-window.c) is exactly this in ~90 lines with no libc (raw syscalls); read it first.

## 5. Keyboard input

* The compositor sends `wl_seat.capabilities` (bit 1<<1 = keyboard) on bind; answer with `wl_seat.get_keyboard(new_id)`.
* `wl_keyboard` events: `keymap`, `enter`, `leave`, `key(serial, time, key, state)`, `modifiers`, `repeat_info(rate, delay)`.
  `key` is a **Linux evdev keycode**, `state` 1 = pressed. Layout is US; `wlterm2` maps keycodes itself and tracks Shift/Ctrl/Alt itself
  (no xkbcommon).
* **Key repeat is yours to do:** start a timer on press, stop on release or `leave`, use the rate/delay from `repeat_info`.
* Tested input is a USB/Bluetooth keyboard through `wl_keyboard`. Mouse/pointer input and the remote's keys were **not** explored here
  (the remote may be handled by the OS before it reaches a client).

## 6. Raw protocol notes (if you skip libwayland)

The apps here speak the wire protocol directly, which keeps them static and tiny. The things that bit us:

* Message = `object id (u32)`, `(size << 16 | opcode) (u32)`, then args, little endian, padded to 4 bytes. Strings: `len incl. NUL`, bytes, pad.
* **New object ids must be allocated sequentially** from the client side (2, 3, 4, ...). Skipping ahead gets
  `invalid arguments for ...` in a `wl_display.error` event, and then the connection is gone.
* `wl_registry.bind` carries the interface *name string and version* before the new id.
* **Ignore `SIGPIPE`.** If the compositor drops you, your next `write` otherwise kills the process before you read the error event
  explaining why. Read events on `object 1, opcode 0` (`wl_display.error`) and print them.
* Do the first roundtrip with `wl_display.sync` (id 3 -> `wl_callback.done`) to be sure all globals arrived before binding.
* A `wl_shm` pool fd is passed with `sendmsg`+`SCM_RIGHTS`; the 16-byte `create_pool` message and the fd go in the same call.

If you'd rather use libwayland, the 64-bit side has none; the 32-bit side uses the TV's `libwayland-client.so.0` and
`libwayland-webos-client.so.1` (for `wl_webos_shell_interface`), as `gpu/wlgl.c` does.

## 7. Build, deploy, run

```sh
# 64-bit CPU app
aarch64-linux-gnu-gcc -static -O2 -o myapp myapp.c -lm
# 32-bit app (freestanding or against the TV's libs; see Makefile targets build/shm-window and build/wlgl)
export TV_IP=<address of your TV>
scripts/deploy.sh myapp /media/developer/bin/myapp
scripts/tv.sh 'XDG_RUNTIME_DIR=/tmp/xdg setsid /media/developer/bin/myapp </dev/null >/tmp/myapp.log 2>&1 &'
```

* **Run detached** (`setsid`, no inherited tty); a child of your telnet session dies when it ends.
* Replacing a running binary fails with "text file busy": stop the app first.
* Telnet moves ~1-2 KB/s; `scripts/deploy.sh` uses a temporary local HTTP server so the TV downloads files at LAN speed.
* Start as **root** (the Homebrew Channel's `exec` service runs as root); the socket directory is `root:composit`, mode 0770.

## 8. Debug loop

| Question | Where |
|---|---|
| Did the compositor accept/hide my window? | `grep NL_VSC /tmp/var/log/messages \| tail` (`app_id`, `visible`) |
| Did the app die and why? | your own log file; `strace` from the Alpine userland ([fish.md](fish.md)); `wl_display.error` text |
| What is on screen? | `scripts/shot.sh out.png` (needs the app to implement a screenshot hook; `wlterm2` does via its control fifo) |
| Drive it without a keyboard | add a control fifo like `wlterm2`'s `/tmp/wlterm2.ctl` (`in`, `text`, `shot`, `quit`) |
| Is the compositor even alive? | run `build/wlglobals` |

## 9. A home-screen icon

Native apps launched through webOS' app manager run inside a sandbox and lose root access, so the launcher is a tiny **web app** that
asks the Homebrew Channel service to run your binary as root:

```js
var b = new PalmServiceBridge();
b.onservicecallback = function (m) { if (JSON.parse(m).returnValue) setTimeout(function () { window.close(); }, 900); };
b.call('luna://org.webosbrew.hbchannel.service/exec', JSON.stringify({ command: '/media/developer/bin/myapp-launch' }));
```

Copy [`app/`](../app) and change the ids. Two things that are easy to get wrong:

* **Relaunch:** the page stays loaded while your native window is in front, so a second click on the icon does not reload it.
  Run the launch on load *and* on the `webOSRelaunch` event, and set `"handlesRelaunch": true` in `appinfo.json`; otherwise the second click
  only flashes the card and drops to Home. Your launch script must also handle "already running" (this repo's restarts it).
* **IPK container:** the `ar` members must be named without the trailing `/` that GNU `ar` adds, or the installer reports
  "IPK Extraction Failure". `app/build-ipk.sh` writes the archive itself.

Install: `luna-send -i -f luna://com.webos.appInstallService/dev/install '{"id":"<id>","ipkUrl":"<path to ipk>","subscribe":true}'`.

## 10. Limits and open questions

* One fullscreen `CARD` window per app id; other window types, multiple windows, popups and overlays are untested.
* The remote's keys, mouse/pointer and audio output (the TV's audio goes through its own services) are not covered here.
* The opcode numbers and property names are specific to webOS 7.x; check `NL_VSC` after any firmware update.
* This modifies a TV as root: keep to writable overlays and `/media/developer`, never to kernel/rootfs partitions
  (<https://rootmy.tv/warning>).
