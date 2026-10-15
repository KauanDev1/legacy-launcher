#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds LegacyRecomp-x86_64.AppImage and LegacyRecomp-<version>-source.tar.gz (docs/packaging.md).
#
#   packaging/appimage/build.sh [--out DIR] [--jobs N]
#
# Everything that runs on the user's machine is compiled with the pinned zig (toolchain.lock) for
# x86_64-linux-gnu.2.31, with libc++ linked statically: the launchers, XenonAnalyse/XenonRecomp, the runtime
# libraries, Ninja and SDL2. The same zig, with cmake, ninja and the static SDL2, goes into
# usr/share/legacyrecomp/toolchain/ so the build stage can compile the game without any system compiler.
#
# Downloads are cached in packaging/appimage/cache/ (git-ignored) and checked against toolchain.lock before
# they are unpacked. Re-runnable: build directories are kept, the AppDir is rebuilt from scratch.
# LR_VERSION overrides the version in the file names (default 0.1.0-m1).
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$here/out
jobs=$(nproc)
version=${LR_VERSION:-0.1.0-m1}
glibc_target=2.31

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'build.sh: error: %s\n' "$*" >&2; exit 1; }

while [ "$#" -gt 0 ]; do
  case $1 in
    --out) [ "$#" -ge 2 ] || die "--out needs a directory"; out=$2; shift 2 ;;
    --jobs) [ "$#" -ge 2 ] || die "--jobs needs a number"; jobs=$2; shift 2 ;;
    -h|--help) sed -n '3,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done
mkdir -p "$out"
out=$(cd "$out" && pwd)

for tool in git curl tar xz gzip python3 sha256sum readelf; do
  command -v "$tool" > /dev/null || die "$tool is required on the build machine"
done

cache=$here/cache
lock=$here/toolchain.lock
mkdir -p "$cache/downloads" "$cache/src" "$cache/build"
export ZIG_GLOBAL_CACHE_DIR=$cache/zig-global-cache
export ZIG_LOCAL_CACHE_DIR=$cache/zig-local-cache

# --- pinned downloads ------------------------------------------------------------------------------------------

lock_get() {  # lock_get NAME COLUMN
  awk -v n="$1" -v c="$2" '$1 == n { print $c; found = 1 } END { exit !found }' "$lock" ||
    die "toolchain.lock has no entry for $1"
}

# fetch NAME: prints the path of the verified download. A new download goes into a fresh directory and is only
# moved into the cache after its SHA-256 matched.
fetch() {
  local name=$1 version sha url file dir tmp
  version=$(lock_get "$name" 2)
  sha=$(lock_get "$name" 3)
  url=$(lock_get "$name" 4)
  file=${url##*/}
  dir=$cache/downloads/$name-$version
  if [ -f "$dir/$file" ] && echo "$sha  $dir/$file" | sha256sum -c --status; then
    echo "$dir/$file"
    return
  fi
  log "downloading $name $version"
  tmp=$(mktemp -d "$cache/downloads/.new-$name.XXXXXX")
  curl -fL --retry 3 --proto '=https' -o "$tmp/$file" "$url" >&2 || die "download failed: $url"
  if ! echo "$sha  $tmp/$file" | sha256sum -c --status; then
    rm -rf "$tmp"
    die "SHA-256 mismatch for $url (expected $sha)"
  fi
  rm -rf "$dir"
  mv "$tmp" "$dir"
  echo "$dir/$file"
}

# unpack NAME ARCHIVE: extracts a verified archive once into a fresh cache/src/NAME-VERSION-SHA directory and
# prints the single top-level directory inside it.
unpack() {
  local name=$1 archive=$2 sha dest
  sha=$(lock_get "$name" 3)
  dest=$cache/src/$name-$(lock_get "$name" 2)-${sha:0:12}
  if [ ! -f "$dest/.unpacked" ]; then
    log "unpacking $name"
    rm -rf "$dest"
    mkdir -p "$dest"
    tar -xf "$archive" -C "$dest" --no-same-owner
    touch "$dest/.unpacked"
  fi
  local top
  top=$(find "$dest" -mindepth 1 -maxdepth 1 -type d)
  [ "$(printf '%s\n' "$top" | wc -l)" -eq 1 ] || die "$archive does not have a single top-level directory"
  echo "$top"
}

# Zig's minisign signature: Ed25519 over the BLAKE2b-512 hash of the tarball ("ED" prehashed signatures).
verify_zig_minisign() {
  local tarball=$1 sig key tmp
  if ! command -v openssl > /dev/null; then
    log "openssl not found: skipping the zig minisign check (SHA-256 was checked)"
    return
  fi
  key=$(lock_get zig-minisign-key 2)
  sig=$tarball.minisig
  if [ ! -f "$sig" ]; then
    curl -fsSL --retry 3 --proto '=https' -o "$sig.tmp" "$(lock_get zig 4).minisig" || die "cannot download $sig"
    mv "$sig.tmp" "$sig"
  fi
  tmp=$(mktemp -d)
  # Public key: "Ed" + 8-byte key id + 32-byte key. Signature line: "ED" + key id + 64-byte signature.
  printf '%s' "$key" | base64 -d | tail -c 32 > "$tmp/pk.raw"
  { printf '\x30\x2a\x30\x05\x06\x03\x2b\x65\x70\x03\x21\x00'; cat "$tmp/pk.raw"; } > "$tmp/pk.der"
  sed -n 2p "$sig" | base64 -d > "$tmp/sig.bin"
  [ "$(head -c 2 "$tmp/sig.bin")" = ED ] || die "unexpected minisign signature algorithm in $sig"
  [ "$(head -c 10 "$tmp/sig.bin" | tail -c 8 | od -An -tx1)" = "$(printf '%s' "$key" | base64 -d | head -c 10 |
    tail -c 8 | od -An -tx1)" ] || die "zig signature made with another key"
  tail -c 64 "$tmp/sig.bin" > "$tmp/sig.raw"
  openssl dgst -blake2b512 -binary "$tarball" > "$tmp/msg.bin"
  openssl pkey -pubin -inform DER -in "$tmp/pk.der" -out "$tmp/pk.pem" 2> /dev/null
  if ! openssl pkeyutl -verify -pubin -inkey "$tmp/pk.pem" -rawin -in "$tmp/msg.bin" -sigfile "$tmp/sig.raw" \
    > /dev/null 2>&1; then
    rm -rf "$tmp"
    die "zig minisign signature does not verify"
  fi
  rm -rf "$tmp"
  log "zig minisign signature verified"
}

zig_tar=$(fetch zig)
verify_zig_minisign "$zig_tar"
zig_src=$(unpack zig "$zig_tar")
cmake_src=$(unpack cmake "$(fetch cmake)")
ninja_src=$(unpack ninja-src "$(fetch ninja-src)")
sdl2_src=$(unpack sdl2 "$(fetch sdl2)")
appimagetool=$(fetch appimagetool)
runtime=$(fetch runtime)
chmod +x "$appimagetool"

# --- AppDir and the bundled toolchain ----------------------------------------------------------------------------

appdir=$out/AppDir
res=$appdir/usr/share/legacyrecomp
tc=$res/toolchain
log "assembling $appdir"
rm -rf "$appdir"
mkdir -p "$tc/bin" "$tc/zig" "$tc/cmake/bin" "$tc/cmake/share" "$appdir/usr/bin"

# zig, trimmed to what x86_64-linux-gnu needs: no other libcs, no headers of other targets, no TSan, no docs.
cp -a "$zig_src/zig" "$zig_src/LICENSE" "$tc/zig/"
mkdir -p "$tc/zig/lib/libc/include"
for d in "$zig_src"/lib/*; do
  case $(basename "$d") in
    libc|libtsan|docs) ;;
    *) cp -a "$d" "$tc/zig/lib/" ;;
  esac
done
cp -a "$zig_src/lib/libc/glibc" "$tc/zig/lib/libc/"
for d in generic-glibc x86-linux-gnu x86-linux-any any-linux-any; do
  cp -a "$zig_src/lib/libc/include/$d" "$tc/zig/lib/libc/include/"
done

# cmake: the binary, its modules and templates, and the license files; no GUI, docs or other tools.
cmake_ver_dir=$(basename "$(find "$cmake_src/share" -maxdepth 1 -name 'cmake-*' -type d)")
cp -a "$cmake_src/bin/cmake" "$tc/cmake/bin/"
mkdir -p "$tc/cmake/share/$cmake_ver_dir" "$tc/cmake/doc/cmake"
for d in Modules Templates Licenses; do
  cp -a "$cmake_src/share/$cmake_ver_dir/$d" "$tc/cmake/share/$cmake_ver_dir/"
done
(cd "$cmake_src/doc/cmake" && find . -type f ! -path './html/*' ! -name '*.qch' -exec cp --parents {} "$tc/cmake/doc/cmake/" \;)

write_wrapper() {  # write_wrapper NAME ZIG-SUBCOMMAND [ARGS...]
  local name=$1
  shift
  cat > "$tc/bin/$name" <<EOF
#!/bin/sh
# SPDX-License-Identifier: MIT
# LegacyRecomp AppImage toolchain: runs \`zig $*\` from ../zig (see docs/packaging.md).
exec "\${0%/*}/../zig/zig" $* "\$@"
EOF
  chmod 755 "$tc/bin/$name"
}
write_wrapper cc cc -target "x86_64-linux-gnu.$glibc_target"
write_wrapper c++ c++ -target "x86_64-linux-gnu.$glibc_target"
write_wrapper ar ar
write_wrapper ranlib ranlib

cc=$tc/bin/cc
cxx=$tc/bin/c++
cmake=$tc/cmake/bin/cmake
zig_cmake_args=(-DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_AR="$tc/bin/ar"
  -DCMAKE_RANLIB="$tc/bin/ranlib")

# --- Ninja (built with zig, so it needs no libstdc++ on the user's machine) -------------------------------------

# Ninja's own bootstrap (configure.py --bootstrap) needs only python3 and a C++ compiler; it writes into its
# directory, so it runs on a copy of the unpacked source.
if [ ! -x "$cache/build/ninja/ninja" ]; then
  log "building ninja"
  rm -rf "$cache/build/ninja"
  cp -a "$ninja_src" "$cache/build/ninja"
  (cd "$cache/build/ninja" && CXX="$cxx" LDFLAGS=-Wl,--strip-debug python3 -I configure.py --bootstrap) > "$cache/build/ninja.log" 2>&1 ||
    die "ninja build failed, see $cache/build/ninja.log"
fi
install -m 755 "$cache/build/ninja/ninja" "$tc/bin/ninja"
ninja=$tc/bin/ninja

# --- SDL2 (static, position independent) ------------------------------------------------------------------------
# -idirafter /usr/include lets SDL find the build machine's X11/Wayland/audio headers for its dlopen'ed backends,
# after zig's glibc 2.31 headers (the glibc check at the end catches anything that slipped through).

log "building SDL2"
"$cmake" -S "$sdl2_src" -B "$cache/build/sdl2" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_BUILD_TYPE=Release \
  "${zig_cmake_args[@]}" -DCMAKE_C_FLAGS="-idirafter /usr/include" -DCMAKE_INSTALL_PREFIX="$tc/sdl2" \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF > "$cache/build/sdl2.log" 2>&1 ||
  die "SDL2 configure failed, see $cache/build/sdl2.log"
"$cmake" --build "$cache/build/sdl2" -j "$jobs" >> "$cache/build/sdl2.log" 2>&1 ||
  die "SDL2 build failed, see $cache/build/sdl2.log"
"$cmake" --install "$cache/build/sdl2" >> "$cache/build/sdl2.log" 2>&1 || die "SDL2 install failed"
# sdl2-config and the .pc file carry build-machine paths and are not used by the game template.
rm -rf "$tc/sdl2/bin" "$tc/sdl2/lib/pkgconfig" "$tc/sdl2/share/aclocal"

# --- LegacyRecomp ------------------------------------------------------------------------------------------------
# The release preset, with zig instead of the system clang and Release (no debug info) instead of RelWithDebInfo.
# -include cstdlib: XenonRecomp's bundled {fmt} relies on <cstdlib> arriving through other libstdc++ headers,
# which libc++ does not do. -idirafter /usr/include: imgui's SDL2 backend includes SDL_syswm.h, which includes
# the X11 headers. --strip-debug: zig's cached libc++ carries debug info (about 20 MB per launcher).

lr_build=$cache/build/legacyrecomp
log "building LegacyRecomp"
(cd "$repo" && "$cmake" --preset release -B "$lr_build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$ninja" \
  "${zig_cmake_args[@]}" -DCMAKE_CXX_FLAGS="-include cstdlib -idirafter /usr/include" \
  -DCMAKE_PREFIX_PATH="$tc/sdl2" -DSDL2_DIR="$tc/sdl2/lib/cmake/SDL2" -DCMAKE_EXE_LINKER_FLAGS=-Wl,--strip-debug \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$lr_build/bin") > "$cache/build/legacyrecomp.log" 2>&1 ||
  die "LegacyRecomp configure failed, see $cache/build/legacyrecomp.log"
"$cmake" --build "$lr_build" -j "$jobs" --target legacyrecomp legacyrecomp-gui lr_resources \
  >> "$cache/build/legacyrecomp.log" 2>&1 || die "LegacyRecomp build failed, see $cache/build/legacyrecomp.log"
[ -x "$lr_build/bin/legacyrecomp-gui" ] || die "legacyrecomp-gui was not built (SDL2 not found?)"
"$cmake" --install "$lr_build" --prefix "$appdir/usr" >> "$cache/build/legacyrecomp.log" 2>&1 ||
  die "cmake --install failed, see $cache/build/legacyrecomp.log"

# --- desktop integration, licenses -------------------------------------------------------------------------------

install -m 755 "$here/AppRun" "$appdir/AppRun"
install -m 644 "$here/legacyrecomp.desktop" "$appdir/legacyrecomp.desktop"
install -m 644 "$here/legacyrecomp.png" "$appdir/legacyrecomp.png"
ln -s legacyrecomp.png "$appdir/.DirIcon"
install -D -m 644 "$here/legacyrecomp.desktop" "$appdir/usr/share/applications/legacyrecomp.desktop"
install -D -m 644 "$here/legacyrecomp.png" "$appdir/usr/share/icons/hicolor/256x256/apps/legacyrecomp.png"

doc=$appdir/usr/share/doc/legacyrecomp
mkdir -p "$doc"
"$here/licenses.sh" "$doc/THIRD-PARTY-LICENSES" "$version" "$repo" "$zig_src" "$cmake_src" "$ninja_src" "$sdl2_src"
cp "$doc/THIRD-PARTY-LICENSES" "$appdir/THIRD-PARTY-LICENSES"
cp "$repo/LICENSE" "$doc/LICENSE"

# --- checks --------------------------------------------------------------------------------------------------------
# Every ELF we built must run on glibc 2.31 and need nothing beyond glibc (no libstdc++, no SDL .so).

log "checking the binaries"
for f in "$appdir/usr/bin/legacyrecomp" "$appdir/usr/bin/legacyrecomp-gui" "$res/tools/XenonAnalyse" \
  "$res/tools/XenonRecomp" "$ninja"; do
  [ -x "$f" ] || die "missing $f"
  newest=$(readelf -V --wide "$f" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)
  if [ -n "$newest" ] && [ "$(printf '%s\n%s\n' "$newest" "$glibc_target" | sort -V | tail -n 1)" != "$glibc_target" ]
  then
    die "${f#"$appdir"/} needs glibc $newest (target $glibc_target)"
  fi
  bad=$(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' |
    grep -Ev '^(libc|libm|libdl|libpthread|librt|ld-linux-x86-64)\.so\.[0-9]+$' || true)
  [ -z "$bad" ] || die "${f#"$appdir"/} needs libraries outside glibc: $bad"
  log "  ${f#"$appdir"/}: glibc ${newest:-none}"
done

# --- AppImage ------------------------------------------------------------------------------------------------------

image=$out/LegacyRecomp-x86_64.AppImage
log "packing $image"
rm -f "$image"
# APPIMAGE_EXTRACT_AND_RUN: appimagetool is itself an AppImage; this works without FUSE.
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$appimagetool" --no-appstream --runtime-file "$runtime" \
  "$appdir" "$image" > "$cache/build/appimagetool.log" 2>&1 ||
  die "appimagetool failed, see $cache/build/appimagetool.log"
chmod +x "$image"

# --- source archive ------------------------------------------------------------------------------------------------
# git archive of HEAD plus every submodule (GitHub's tag tarballs leave the submodules out), under one prefix.

source_tar=$out/LegacyRecomp-$version-source.tar.gz
log "writing $source_tar"
if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]; then
  log "warning: the work tree has uncommitted changes; the source archive contains HEAD only"
fi
prefix=LegacyRecomp-$version/
tmp=$(mktemp -d "$cache/build/.source.XXXXXX")
git -C "$repo" archive --format=tar --prefix="$prefix" -o "$tmp/all.tar" HEAD
git -C "$repo" submodule --quiet foreach --recursive \
  "git archive --format=tar --prefix=\"$prefix\$displaypath/\" -o \"$tmp/sub.tar\" HEAD &&
   tar --concatenate -f \"$tmp/all.tar\" \"$tmp/sub.tar\""
gzip -n -9 < "$tmp/all.tar" > "$source_tar"
rm -rf "$tmp"

log "done:"
ls -l "$image" "$source_tar" >&2
