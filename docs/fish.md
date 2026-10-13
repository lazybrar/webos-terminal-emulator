# Getting the fish shell on the TV

webOS ships only busybox `sh`: no completion, no history search, no syntax colors. [fish](https://fishshell.com) fixes all of that with
zero configuration, and `wlterm2` already prefers it: the default shell is `/usr/alpine/usr/bin/fish` when it exists, else `/bin/sh`.

fish comes from a small **64-bit Alpine Linux userland** installed under `/usr/alpine`. It does not touch the stock system, and it also
gives you `vim`, `git`, `tmux`, `htop`, `curl`, `strace`... (about 87 MB for 52 packages).

> Requirements: a rooted TV with the persistent writable overlay from [system.md](system.md#persistent-writable-usr-lib-etc)
> installed (`overlay/10-rwoverlay`), and about 100 MB free on the data partition (`df /media/developer`).
> Everything below is typed **on the TV** (telnet / the terminal itself), as root. The TV downloads everything itself.

## 1. Install the minimal Alpine root

```sh
cd /media/developer && mkdir -p a64boot && cd a64boot

# apk-tools-static bootstraps everything else. Find the current file name in the directory listing:
#   https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/aarch64/   (look for apk-tools-static-<version>.apk)
curl -sSfLO https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/aarch64/apk-tools-static-<version>.apk
gzip -dc apk-tools-static-*.apk | tar -x            # yields sbin/apk.static (busybox tar: no -i needed)

R=/usr/alpine; mkdir -p $R/etc/apk
printf 'https://dl-cdn.alpinelinux.org/alpine/latest-stable/main\nhttps://dl-cdn.alpinelinux.org/alpine/latest-stable/community\n' > $R/etc/apk/repositories

# the first install has no keys yet, hence --allow-untrusted (only for the base packages that bring the keys)
./sbin/apk.static --root $R --initdb --arch aarch64 --allow-untrusted \
    add alpine-baselayout busybox musl apk-tools alpine-keys ca-certificates-bundle
```

## 2. Install fish (signature-checked this time)

```sh
./sbin/apk.static --root $R --arch aarch64 add fish
# optional extras:
./sbin/apk.static --root $R --arch aarch64 add bash vim git tmux htop nano less strace curl file lsof xz
```

## 3. Make the 64-bit programs runnable

Alpine's programs are dynamic **musl** binaries whose interpreter path is `/lib/ld-musl-aarch64.so.1`. That name does not collide with the
TV's glibc loader (`/lib/ld-linux.so.3`), so just put the loader where they expect it and tell musl where the libraries are:

```sh
cp $R/lib/ld-musl-aarch64.so.1 /lib/
echo /usr/alpine/lib:/usr/alpine/usr/lib > /etc/ld-musl-aarch64.path
```

(`/lib` and `/etc` must be the writable, persistent overlays; that is why the overlay hook is a prerequisite.)

Check it:

```sh
/usr/alpine/usr/bin/fish -c 'echo fish $version'
```

## 4. Use it

Restart the terminal from the home-screen icon (or `wlterm-launch`): it starts fish. Nothing to configure: autosuggestions (right arrow
accepts), tab completion, syntax highlighting, `ctrl-r` history.

`wlterm2` already puts `/usr/alpine/usr/bin:/usr/alpine/bin` **after** the stock directories in `PATH`, so the TV's own busybox keeps
priority and Alpine tools fill the gaps. To make telnet / ssh logins find them as well, add the same to the persisted `/etc/profile`.

Use another shell or pin fish explicitly in `/media/developer/wlterm.conf`:

```
shell = /usr/alpine/usr/bin/fish
```

Installing more later: `/media/developer/a64boot/sbin/apk.static --root /usr/alpine --arch aarch64 add <package>` (or `apk --root`
once Alpine's own `apk` is on your path). Search with `apk.static --root /usr/alpine search <name>`.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `fish: not found` or `No such file or directory` when the file exists | The musl loader is missing: redo step 3 (`ls -l /lib/ld-musl-aarch64.so.1`). |
| `Error loading shared library libpcre2-8.so.0` (or any `lib*.so`) | `/etc/ld-musl-aarch64.path` is missing, or the writable `/etc` overlay is not active after a reboot: `touch /etc/x && rm /etc/x` must succeed (see [system.md](system.md)). |
| Terminal opens with `sh` instead of fish | fish is not at `/usr/alpine/usr/bin/fish`, or the `shell =` setting points elsewhere. |
| `apk` complains about signatures | Make sure step 2 ran without `--allow-untrusted` and `alpine-keys` installed in step 1. |
| Out of space | `df /media/developer`; drop optional packages, `apk.static --root /usr/alpine del <pkg>`, delete `/media/developer/a64boot`. |
| Weird glyphs in the prompt | Add a Nerd Font as a `fallback =` font (see [terminal.md](terminal.md)). |

## Undo

```sh
rm -rf /usr/alpine /lib/ld-musl-aarch64.so.1 /etc/ld-musl-aarch64.path /media/developer/a64boot
```

The terminal falls back to `/bin/sh`.
