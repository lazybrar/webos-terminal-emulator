# Making GPU (PowerVR / GLES2) apps for webOS

## Why only 32-bit apps get the GPU

The TV's kernel is aarch64, but everything in user space, including the PowerVR driver (`libEGL.so.1`, `libGLESv2.so.2`,
`libwayland-egl.so.1`, the `*_PVR_MESA` libraries), is **32-bit ARM, soft-float ABI, glibc 2.31**. A 64-bit process cannot load them,
and there is no 64-bit PowerVR driver or Mesa driver for this GPU. So:

* GPU app: a 32-bit ARM executable, dynamically linked against the TV's own libraries.
* CPU-only tool: anything, including 64-bit static binaries (see [system.md](system.md)).

## What the compositor offers

Run `build/wlglobals` on the TV. Typical output: `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_drm`, `zwp_linux_dmabuf_v1`,
`wl_seat`, `wl_shell`, `wl_output` and LG's `wl_webos_shell`, `wl_webos_surface_group_compositor`, `wl_webos_input_manager`, `wl_starfish_*`.
There is **no `xdg_shell`**: toolkits that only speak `xdg_shell` (GTK4, recent Qt, foot, alacritty) need a shim.

## Making a window that stays

A surface created through plain `wl_shell` is closed by the compositor after about a second. `surface-manager` only keeps windows that
announce an app id. Recipe (works on webOS 7.5.8; check `grep NL_VSC /tmp/var/log/messages`, which logs
`{"app_id":"...","visible":true,...}` once the window is accepted):

1. Bind `wl_webos_shell`, call `get_shell_surface(new_id, surface)` (request opcode 1).
2. On the returned `wl_webos_shell_surface` call `set_property(name, value)` (opcode 2):
   `appId` = your id, `_WEBOS_WINDOW_TYPE` = `_WEBOS_WINDOW_TYPE_CARD`, `instanceId` = your id.
3. Call `set_state(3)` (opcode 1) for fullscreen.

The opcodes come from the order of the messages in `libwayland-webos-client.so`; the property names were found empirically.

## EGL path

`gpu/wlgl.c` does this in about 150 lines with **no libc and no headers**: it declares the handful of functions it needs and links
against the TV's libraries.

1. `wl_display_connect`, get the registry, bind `wl_compositor` and `wl_webos_shell` (use `wl_proxy_marshal_constructor_versioned`
   with the interface symbols `wl_registry_interface`, `wl_compositor_interface`, ... exported by `libwayland-client` and
   `wl_webos_shell_interface` from `libwayland-webos-client`).
2. Create the surface and the webOS shell surface (recipe above), then `wl_egl_window_create(surface, w, h)`.
3. `eglGetDisplay(wl_display)`, `eglInitialize`, `eglBindAPI(EGL_OPENGL_ES_API)`, `eglChooseConfig` (window + ES2 + RGBA8),
   `eglCreateContext(client version 2)`, `eglCreateWindowSurface`, `eglMakeCurrent`, `eglSwapInterval(1)`.
4. Render with GLES2 and `eglSwapBuffers` each frame, then `wl_display_dispatch_pending` + `wl_display_flush`.

### Build it

```sh
export TV_IP=<address of your TV>
scripts/pull-sysroot.sh sysroot      # copies the TV's libs here, used only for linking
make gpu                             # clang --target=arm-linux-gnueabi -mfloat-abi=softfp ... -l:libEGL.so.1 ...
scripts/deploy.sh build/wlgl /media/developer/bin/wlgl
scripts/tv.sh 'XDG_RUNTIME_DIR=/tmp/xdg setsid /media/developer/bin/wlgl </dev/null >/tmp/wlgl.log 2>&1 &'
```

Details that matter:

* `-mfloat-abi=softfp` (arguments in core registers, VFP for the math) matches the TV's libraries; link with `-Wl,--dynamic-linker=/lib/ld-linux.so.3`.
* Use `-march=armv7ve` so integer division is a hardware instruction (otherwise you need `__aeabi_uidiv`).
* With `-nostdlib` you provide `_start`; the TV's `libc.so.6` is still loaded and initialised by the dynamic loader because the EGL
  libraries depend on it. Exit with the `exit_group` syscall. If you want normal C, install a 32-bit softfp glibc 2.31 sysroot
  (for example Debian bullseye armel) and link normally.
* Wayland object ids must be allocated **sequentially**: the server rejects an id that skips ahead ("invalid arguments for ...").
* Ignore `SIGPIPE` in clients that talk raw Wayland; when the compositor drops you, the next `write` would otherwise kill you before you read its error event.

## Other ways to use the GPU

* Other toolkits (SDL2 with the GLES backend, Qt with the webOS platform plugin `libwebos-wayland-egl`) already exist on the TV in
  32-bit form; they must be given the same `appId` via `wl_webos_shell`. Not tested here.
* For pure CPU drawing, `examples/shm-window.c` shows the minimum `wl_shm` + `wl_webos_shell` window without libc.
