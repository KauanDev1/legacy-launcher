#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds LegacyRecomp-x86_64.AppImage and LegacyRecomp-<version>-source.tar.gz (docs/packaging.md).
#
#   packaging/appimage/build.sh [--out DIR] [--jobs N] [--allow-dirty]
#
# Everything that runs on the user's machine is compiled with the pinned zig (toolchain.lock) for
# x86_64-linux-gnu.2.31, with libc++ linked statically: the launchers, XenonAnalyse/XenonRecomp, the runtime
# libraries, Ninja and SDL2. The same zig, with cmake, ninja and the static SDL2, goes into
# usr/share/legacyrecomp/toolchain/ so the build stage can compile the game without any system compiler.
#
# Downloads are cached in packaging/appimage/cache/ (git-ignored) and checked against toolchain.lock before
# they are unpacked. Re-runnable: build directories are kept, the AppDir is rebuilt from scratch.
# LR_VERSION overrides the version in the file names (default 0.1.0-m1). The source archive is made from HEAD,
# so a work tree with uncommitted changes is refused unless --allow-dirty is given.
# Also writes LegacyRecomp-<version>-runtime-sources.tar.gz: the source of the AppImage runtime and of the
# libfuse it links statically (LGPL-2.1), as pinned in toolchain.lock.
set -euo pipefail
shopt -s inherit_errexit

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$here/out
jobs=$(nproc)
allow_dirty=0
version=${LR_VERSION:-0.1.0-m1}
glibc_target=2.31

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'build.sh: error: %s\n' "$*" >&2; exit 1; }

while [ "$#" -gt 0 ]; do
  case $1 in
    --out) [ "$#" -ge 2 ] || die "--out needs a directory"; out=$2; shift 2 ;;
    --jobs) [ "$#" -ge 2 ] || die "--jobs needs a number"; jobs=$2; shift 2 ;;
    --allow-dirty) allow_dirty=1; shift ;;
    -h|--help) sed -n '3,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done
mkdir -p "$out"
out=$(cd "$out" && pwd)

for tool in git curl tar xz gzip python3 sha256sum readelf; do
  command -v "$tool" > /dev/null || die "$tool is required on the build machine"
done
if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]; then
  [ "$allow_dirty" = 1 ] ||
    die "the work tree has uncommitted changes; the source archive would not match the AppImage (commit them, or pass --allow-dirty for a test build)"
  log "warning: building with uncommitted changes (--allow-dirty); the source archive contains HEAD only"
fi

# Temporary directories removed on exit, also when die() stops the script.
cleanup_dirs=()
cleanup() { [ "${#cleanup_dirs[@]}" -eq 0 ] || rm -rf "${cleanup_dirs[@]}"; }
trap cleanup EXIT

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
  if ! curl -fL --retry 3 --proto '=https' -o "$tmp/$file" "$url" >&2; then
    rm -rf "$tmp"
    die "download failed: $url"
  fi
  if ! echo "$sha  $tmp/$file" | sha256sum -c --status; then
    rm -rf "$tmp"
    die "SHA-256 mismatch for $url (expected $sha)"
  fi
  rm -rf "$dir"
  mv "$tmp" "$dir"
  echo "$dir/$file"
}

# unpack NAME ARCHIVE: extracts a verified archive once into cache/src/NAME-VERSION-SHA and prints the single
# top-level directory inside it. Extraction goes to a temporary directory that is renamed only after tar
# succeeded, so a failed or interrupted extraction is never taken for a finished one.
unpack() {
  local name=$1 archive=$2 sha dest tmp top
  [ -f "$archive" ] || die "unpack $name: no archive"
  sha=$(lock_get "$name" 3)
  dest=$cache/src/$name-$(lock_get "$name" 2)-${sha:0:12}
  if [ ! -f "$dest/.unpacked" ]; then
    log "unpacking $name"
    rm -rf "$dest"
    tmp=$(mktemp -d "$cache/src/.new-$name.XXXXXX")
    if ! tar -xf "$archive" -C "$tmp" --no-same-owner; then
      rm -rf "$tmp"
      die "cannot extract $archive"
    fi
    touch "$tmp/.unpacked"
    mv "$tmp" "$dest"
  fi
  top=$(find "$dest" -mindepth 1 -maxdepth 1 -type d)
  [ -n "$top" ] || die "$archive is empty"
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
  tmp=$(mktemp -d "$cache/downloads/.minisign.XXXXXX")
  cleanup_dirs+=("$tmp")
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
  openssl pkeyutl -verify -pubin -inkey "$tmp/pk.pem" -rawin -in "$tmp/msg.bin" -sigfile "$tmp/sig.raw" \
    > /dev/null 2>&1 || die "zig minisign signature does not verify"
  log "zig minisign signature verified"
}

# One assignment per call: a failing fetch or unpack stops the script here.
zig_tar=$(fetch zig)
verify_zig_minisign "$zig_tar"
cmake_tar=$(fetch cmake)
ninja_tar=$(fetch ninja-src)
sdl2_tar=$(fetch sdl2)
libfuse_tar=$(fetch libfuse-src)
runtime_src_tar=$(fetch runtime-src)
appimagetool=$(fetch appimagetool)
runtime=$(fetch runtime)
zig_src=$(unpack zig "$zig_tar")
cmake_src=$(unpack cmake "$cmake_tar")
ninja_src=$(unpack ninja-src "$ninja_tar")
sdl2_src=$(unpack sdl2 "$sdl2_tar")
libfuse_src=$(unpack libfuse-src "$libfuse_tar")
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
# which libc++ does not do.
# -idirafter /usr/include: imgui's SDL2 backend includes SDL_syswm.h, which includes the X11 headers.
# --strip-debug: zig's cached libc++ carries debug info (about 20 MB per launcher).

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
"$here/licenses.sh" "$doc/THIRD-PARTY-LICENSES" "$version" "$repo" "$zig_src" "$cmake_src" "$ninja_src" "$sdl2_src" \
  "$libfuse_src"
cp "$doc/THIRD-PARTY-LICENSES" "$appdir/THIRD-PARTY-LICENSES"
cp "$repo/LICENSE" "$doc/LICENSE"

# --- checks --------------------------------------------------------------------------------------------------------
# Every ELF we built must run on glibc 2.31 and need nothing beyond glibc (no libstdc++, no SDL .so).

log "checking the binaries"
gpl_disasm_marker="attempt to set y bit when using + or - modifier"  # thirdparty/.../disasm/ppc-dis.c
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
  # GPL tripwire (ADR 0002, like the no_gpl_disassembler_in_binaries test): the binutils PowerPC disassembler
  # (GPL-2.0-or-later) may live in XenonAnalyse/XenonRecomp only, never in the MIT launchers. zig leaves no
  # .symtab in these binaries, so besides the decode_insn_ppc symbol (checked when a symbol table exists) we look
  # for one of ppc-dis.c's message strings, which must be present in the tools (else the marker is stale).
  case $f in
    */usr/bin/legacyrecomp | */usr/bin/legacyrecomp-gui)
      syms=$(LC_ALL=C readelf -sW "$f")  # no pipe into grep -q: with pipefail, readelf's SIGPIPE would mask a hit
      ! grep -qw decode_insn_ppc <<< "$syms" ||
        die "${f#"$appdir"/} contains decode_insn_ppc: the GPL-2.0-or-later binutils disassembler got linked in"
      ! grep -qaF "$gpl_disasm_marker" "$f" ||
        die "${f#"$appdir"/} contains the binutils disassembler (\"$gpl_disasm_marker\")"
      ;;
    */tools/XenonAnalyse | */tools/XenonRecomp)
      grep -qaF "$gpl_disasm_marker" "$f" ||
        die "GPL tripwire marker \"$gpl_disasm_marker\" not found in ${f#"$appdir"/}; update the marker"
      ;;
  esac
  log "  ${f#"$appdir"/}: glibc ${newest:-none}"
done
[ ! -e "$res/runtime/lib/libdisasm.a" ] || die "runtime/lib/libdisasm.a is staged; the game link must not get it"

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
prefix=LegacyRecomp-$version/
tmp=$(mktemp -d "$cache/build/.source.XXXXXX")
cleanup_dirs+=("$tmp")
git -C "$repo" archive --format=tar --prefix="$prefix" -o "$tmp/all.tar" HEAD
git -C "$repo" submodule --quiet foreach --recursive \
  "git archive --format=tar --prefix=\"$prefix\$displaypath/\" -o \"$tmp/sub.tar\" HEAD &&
   tar --concatenate -f \"$tmp/all.tar\" \"$tmp/sub.tar\""
gzip -n -9 < "$tmp/all.tar" > "$source_tar"

# --- AppImage runtime sources --------------------------------------------------------------------------------------
# The type2 runtime statically links libfuse 3 (LGPL-2.1). Releases carry its corresponding source: the pinned
# type2-runtime source (its build scripts and the libfuse patch it applies) and the libfuse tarball it builds
# (same version and SHA-256 as its scripts/common/install-dependencies.sh).

runtime_sources=$out/LegacyRecomp-$version-runtime-sources.tar.gz
log "writing $runtime_sources"
rs=$tmp/LegacyRecomp-$version-runtime-sources
mkdir -p "$rs"
runtime_src_name=type2-runtime-$(lock_get runtime-src 2).tar.gz
cp "$runtime_src_tar" "$rs/$runtime_src_name"
cp "$libfuse_tar" "$rs/"
cat > "$rs/README" <<README
Corresponding source for the AppImage runtime in LegacyRecomp-x86_64.AppImage $version.

$runtime_src_name  AppImage type2-runtime $(lock_get runtime-src 2) (MIT): runtime.c, the build
  scripts (scripts/) and the libfuse patch (patches/libfuse/mount.c.diff). The runtime in the AppImage is the
  unmodified release asset runtime-x86_64 of that tag (SHA-256 $(lock_get runtime 3)).
$(basename "$libfuse_tar")  libfuse $(lock_get libfuse-src 2) (LGPL-2.1 for lib/ and include/), the version the
  runtime's scripts/common/install-dependencies.sh builds statically, with the patch above applied.
The other libraries the runtime links (musl, squashfuse, zstd, zlib, mimalloc) are listed in that script;
their licenses are in THIRD-PARTY-LICENSES inside the AppImage.
README
tar -C "$tmp" -cf - "$(basename "$rs")" | gzip -n -9 > "$runtime_sources"

log "done:"
ls -l "$image" "$source_tar" "$runtime_sources" >&2
