#!/bin/sh
# Cross-compile gemu.exe for 64-bit Windows from Linux, and optionally run
# it under Wine.
#
#   tools/build-windows.sh                  # build bin-win/gemu.exe
#   tools/build-windows.sh run -M nes -cartridge roms/smb.nes
#
# Needs the MinGW-w64 cross compiler (Arch: pacman -S mingw-w64-gcc;
# Debian/Ubuntu: apt install gcc-mingw-w64-x86-64) and, for "run", Wine.
# SDL2 and SDL2_ttf come from libsdl.org's official MinGW development
# packages, downloaded once into build-win/deps.  The build lives in
# build-win/ and bin-win/ and leaves the native build.ninja alone.
#
# Built like the Windows CI job without GTK or OpenSSL (--disable-crypto:
# no VNC password authentication).
set -e
cd "$(dirname "$0")/.."

HOST=x86_64-w64-mingw32
SDL_VER=2.30.9
TTF_VER=2.22.0
DEPS=build-win/deps
SYSROOT=$DEPS/$HOST
WINEPREFIX_DIR=build-win/wineprefix

fetch() {   # fetch NAME URL: unpack a MinGW devel package into the sysroot
    [ -f "$DEPS/.$1" ] && return
    echo "fetching $1"
    mkdir -p "$DEPS"
    curl -fsSL "$2" | tar -xz -C "$DEPS"
    cp -r "$DEPS"/"$1"*/"$HOST"/. "$SYSROOT"/
    touch "$DEPS/.$1"
}

build() {
    command -v $HOST-gcc >/dev/null ||
        { echo "build-windows: $HOST-gcc not found - install mingw-w64-gcc" >&2; exit 1; }
    mkdir -p "$SYSROOT"
    fetch SDL2-$SDL_VER \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz"
    fetch SDL2_ttf-$TTF_VER \
        "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$TTF_VER/SDL2_ttf-devel-$TTF_VER-mingw.tar.gz"
    # the packages' .pc files name the prefix they were built in
    sysroot_abs=$(cd "$SYSROOT" && pwd)
    sed -i "s|^prefix=.*|prefix=$sysroot_abs|" "$SYSROOT"/lib/pkgconfig/*.pc

    # configure writes ./build.ninja: keep the native one and park ours
    [ -f build.ninja ] && cp build.ninja build-win/build.ninja.native
    PKG_CONFIG_LIBDIR="$sysroot_abs/lib/pkgconfig" \
        python3 configure --cross=$HOST --disable-crypto \
        --build-dir=build-win/obj --bindir=bin-win
    mv build.ninja build-win/build.ninja
    [ -f build-win/build.ninja.native ] && mv build-win/build.ninja.native build.ninja
    ninja -f build-win/build.ninja

    # the DLLs gemu.exe imports that aren't part of Windows, beside it
    copy_dlls bin-win/gemu.exe
    echo "built bin-win/gemu.exe"
}

copy_dlls() {   # follow import tables through the SDL sysroot and MinGW runtime
    for dll in $($HOST-objdump -p "$1" | awk '/DLL Name:/ {print $3}'); do
        [ -f "bin-win/$dll" ] && continue
        for dir in "$SYSROOT/bin" /usr/$HOST/bin /usr/$HOST/lib; do
            if [ -f "$dir/$dll" ]; then
                cp "$dir/$dll" bin-win/
                copy_dlls "bin-win/$dll"
                break
            fi
        done
    done
}

case "$1" in
run)
    shift
    [ -f bin-win/gemu.exe ] || build
    command -v wine >/dev/null || { echo "build-windows: wine not found" >&2; exit 1; }
    # a prefix of its own, so nothing touches ~/.wine; no Mono/Gecko prompts
    export WINEPREFIX="$(pwd)/$WINEPREFIX_DIR" WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all
    exec wine bin-win/gemu.exe "$@"
    ;;
""|build)
    build
    ;;
*)
    echo "usage: tools/build-windows.sh [build | run GEMU-ARGS...]" >&2
    exit 2
    ;;
esac
