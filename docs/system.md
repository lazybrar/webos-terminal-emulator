# System notes: writable overlay, 64-bit userland, test loop

Everything here was done on webOS 7.5.8. The root filesystem is a read-only squashfs, so the trick is to stack overlays on top of it
instead of replacing anything.

## Persistent writable `/usr`, `/lib`, `/etc`

`overlay/10-rwoverlay` mounts, for each of `/usr`, `/lib`, `/etc`, an overlayfs whose lower dir is the existing directory and whose
upper dir lives on the ext4 data partition (`/media/developer/rw/<dir>/up`). Anything you write goes to the upper dir and survives reboots;
removing the upper dir restores the stock system.

* The stock system already mounts things *inside* these directories (fonts, nyx modules, `/etc/hosts`, ...). The script recursively
  re-binds those sub-mounts into the new overlay and then `mount --rbind`s the result over the original, so they keep working.
  (A plain `--bind` loses them. Never `rm -rf` a path while an old overlay is still mounted under `/tmp`.)
* Install it where the rooted-webOS startup script runs boot hooks: `/var/lib/webosbrew/init.d/` (it calls `run-parts` on that directory
  at every boot):
  ```sh
  scripts/deploy.sh overlay/10-rwoverlay /var/lib/webosbrew/init.d/10-rwoverlay
  scripts/tv.sh 'sh /var/lib/webosbrew/init.d/10-rwoverlay; touch /usr/x && rm /usr/x && echo writable'
  ```
* Undo: delete the hook and `/media/developer/rw`, then reboot. Do not touch the kernel / rootfs / service partitions.

## 64-bit Alpine userland in `/usr/alpine`

Static aarch64 programs run on this kernel, and so do dynamic **musl** ones, provided their loader is where they expect it.
Alpine's musl loader is named `ld-musl-aarch64.so.1`, which does not collide with the TV's glibc loader `ld-linux.so.3`, so a whole
64-bit userland can live next to the stock 32-bit one. It has no GPU access (see [gpu-apps.md](gpu-apps.md)).

Step-by-step with troubleshooting: **[fish.md](fish.md)**. In short, all on the TV (it has curl, wget, tar, gzip, TLS; nothing is staged on the PC):

```sh
cd /media/developer && mkdir -p a64boot && cd a64boot
curl -sSfLO https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/aarch64/apk-tools-static-<version>.apk   # find <version> in the directory listing
gzip -dc apk-tools-static-*.apk | tar -x          # busybox tar: no -i needed; yields sbin/apk.static
R=/usr/alpine; mkdir -p $R/etc/apk
printf 'https://dl-cdn.alpinelinux.org/alpine/latest-stable/main\nhttps://dl-cdn.alpinelinux.org/alpine/latest-stable/community\n' > $R/etc/apk/repositories
./sbin/apk.static --root $R --initdb --arch aarch64 --allow-untrusted add alpine-baselayout busybox musl apk-tools alpine-keys ca-certificates-bundle
./sbin/apk.static --root $R --arch aarch64 add fish bash vim git tmux htop nano less strace curl file lsof xz   # now signature-checked
cp $R/lib/ld-musl-aarch64.so.1 /lib/                                   # where the binaries' PT_INTERP points
echo /usr/alpine/lib:/usr/alpine/usr/lib > /etc/ld-musl-aarch64.path   # musl's library search path (needs the writable /etc overlay)
```

Add `/usr/alpine/usr/bin:/usr/alpine/bin` **after** the stock directories to `PATH` (the hook-persisted `/etc/profile` is a good place), so
webOS' own busybox keeps priority. Packages added later with `apk.static --root /usr/alpine add ...` work the same way.
Free space on the data partition is limited (about 300 MB originally; 52 packages took 87 MB).

## The test loop used to build all this

* `scripts/tv.sh 'cmd'` runs a command over telnet; `scripts/deploy.sh` copies files fast over a temporary local HTTP server;
  `scripts/pull.sh` copies TV -> PC through the telnet stream (base64).
* Window state: `grep NL_VSC /tmp/var/log/messages | tail` shows every window `surface-manager` accepts or hides, by `app_id`.
* Compositor errors arrive as a `wl_display.error` event, for example `invalid arguments for wl_registry@2.bind`. Read it before
  assuming a crash; a client that is not reading the socket just dies of `SIGPIPE`.
* `strace` (from Alpine) works on the TV and is the quickest way to see why a client exits.
