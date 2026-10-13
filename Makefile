# Build on the PC. Needs: aarch64-linux-gnu-gcc, stb (stb_truetype.h), clang + lld, optional resvg/ffmpeg.
CC64     ?= aarch64-linux-gnu-gcc
STRIP64  ?= aarch64-linux-gnu-strip
STB_INC  ?= /usr/include
CLANG    ?= clang
# The TV userland is 32-bit ARM (soft-float ABI) on a 64-bit kernel.
ARM32    = --target=arm-linux-gnueabi -march=armv7ve -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -fuse-ld=lld -O2 -Wall
SYSROOT ?= sysroot

all: build/wlterm2 build/wlglobals build/shm-window
.PHONY: all gpu ipk clean
build: ; mkdir -p build

build/wlterm2: terminal/wlterm2.c | build
	$(CC64) -static -O2 -Wall -idirafter $(STB_INC) -o $@ $< -lm && $(STRIP64) $@
build/wlglobals: examples/wlglobals.c | build
	$(CLANG) $(ARM32) -mfloat-abi=soft -static -o $@ $<
build/shm-window: examples/shm-window.c | build
	$(CLANG) $(ARM32) -mfloat-abi=soft -static -o $@ $<
# GPU example: dynamic, links the TV's own libs (run scripts/pull-sysroot.sh first)
gpu: build/wlgl
build/wlgl: gpu/wlgl.c | build
	$(CLANG) $(ARM32) -mfpu=neon-vfpv4 -mfloat-abi=softfp -Wl,--dynamic-linker=/lib/ld-linux.so.3 -o $@ $< -L$(SYSROOT) \
	  -l:libEGL.so.1 -l:libGLESv2.so.2 -l:libwayland-egl.so.1 -l:libwayland-client.so.0 -l:libwayland-webos-client.so.1 -l:libc.so.6
ipk: build/wlterm2 | build
	app/build-ipk.sh build/org.tvwl.terminal.ipk
clean: ; rm -rf build
