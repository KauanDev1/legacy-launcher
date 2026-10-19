#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Writes THIRD-PARTY-LICENSES for the AppImage: a summary of every component the AppImage contains, the notes on
# the GPL/LGPL combined works (docs/adr/0002-license.md, "Obrigações dos pacotes binários") and the full license
# text and copyright notice of each component.
#
#   licenses.sh OUT VERSION REPO ZIG_DIR CMAKE_DIR NINJA_SRC_DIR SDL2_SRC_DIR LIBFUSE_SRC_DIR
#
# The *_DIR arguments are the extracted pinned downloads (packaging/appimage/build.sh).
set -euo pipefail

if [ "$#" -ne 8 ]; then
  echo "usage: $0 OUT VERSION REPO ZIG_DIR CMAKE_DIR NINJA_SRC_DIR SDL2_SRC_DIR LIBFUSE_SRC_DIR" >&2
  exit 2
fi
out=$1 version=$2 repo=$3 zig=$4 cmake=$5 ninja=$6 sdl2=$7 libfuse=$8
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tp=$repo/thirdparty/XenonRecomp/thirdparty

section() {  # section TITLE FILE...
  local title=$1
  shift
  printf '\n\n%s\n%s\n' "$title" "$(printf '%*s' "${#title}" '' | tr ' ' '=')"
  for f in "$@"; do
    [ -f "$f" ] || { echo "licenses.sh: missing license file $f" >&2; exit 1; }
    printf '\n--- %s ---\n\n' "${f#"$repo"/}"
    cat "$f"
  done
}

header_comment() {  # header_comment FILE LINES: the first LINES lines (the license comment) of a source file
  [ -f "$1" ] || { echo "licenses.sh: missing $1" >&2; exit 1; }
  head -n "$2" "$1"
}

{
cat <<EOF
LegacyRecomp $version AppImage: third-party components and licenses
=====================================================================

LegacyRecomp's own code (launcher, pipeline, runtime) is under the MIT license (first section below).
This AppImage also contains code from other projects, under their own licenses. This file lists every
component, then gives each full license text and copyright notice.

Where things are in the AppImage:
  usr/bin/legacyrecomp, usr/bin/legacyrecomp-gui   the launchers
  usr/share/legacyrecomp/tools/                    XenonAnalyse, XenonRecomp
  usr/share/legacyrecomp/runtime/                  runtime libraries, headers (incl. simde), game template
  usr/share/legacyrecomp/fonts/                    Inter, JetBrains Mono
  usr/share/legacyrecomp/toolchain/                zig, cmake, ninja, static SDL2 (used to build the game)
  (the AppImage file itself)                       the AppImage type2 runtime

Component                          License                                   Where
---------------------------------  ----------------------------------------  ------------------------------------
LegacyRecomp                       MIT                                       launchers, runtime, game template
XenonRecomp/XenonAnalyse/XenonUtils MIT                                      tools; XenonUtils in the launchers
Xenia (code in XenonUtils)         BSD-3-Clause                              launchers, tools (in XenonUtils), note 5
Dear ImGui                         MIT                                       legacyrecomp-gui
volk (v1.4.309)                    MIT                                       not linked yet (M3)
VulkanMemoryAllocator (v3.2.1)     MIT                                       not linked yet (M3)
Vulkan-Headers (1.4.309)           Apache-2.0                                not linked yet (M3)
toml++                             MIT                                       launchers, XenonRecomp
{fmt}                              MIT                                       launchers, tools
xxHash (library)                   BSD-2-Clause                              XenonRecomp
SIMD Everywhere (simde)            MIT                                       runtime/include/simde (headers)
libmspack (lzxd.c only)            LGPL-2.1                                  launchers, tools (static), see note 2
tiny-AES-c                         Unlicense                                 launchers, tools (in XenonUtils)
TinySHA1                           ISC-style                                 launchers, tools (in XenonUtils)
GNU binutils PowerPC disassembler  GPL-2.0-or-later                          XenonAnalyse, XenonRecomp only, note 1
Inter, JetBrains Mono              SIL OFL 1.1                               fonts/
SDL2                               zlib                                      legacyrecomp-gui (static), toolchain/sdl2
Zig                                MIT                                       toolchain/zig, see note 3
LLVM, Clang, LLD, libc++,          Apache-2.0 WITH LLVM-exception            inside toolchain/zig (compiler binary,
  libc++abi, libunwind, compiler-rt                                           lib/libcxx, lib/libcxxabi, ...);
                                                                              libc++ is static in the launchers
GNU C Library headers and sources  LGPL-2.1-or-later + notices in LICENSES   toolchain/zig/lib/libc
Linux kernel UAPI headers          GPL-2.0 WITH Linux-syscall-note           toolchain/zig/lib/libc/include
CMake (and its bundled libraries)  BSD-3-Clause (+ licenses listed below)    toolchain/cmake
OpenSSL 3.6.0                      Apache-2.0                                statically linked in toolchain/cmake
Ninja                              Apache-2.0                                toolchain/bin/ninja
AppImage type2 runtime             MIT                                       the AppImage file header, note 4
  musl, libfuse, squashfuse,       MIT, LGPL-2.1, BSD-2-Clause,
  zstd, zlib, mimalloc             BSD-3-Clause, zlib, MIT

Notes
-----

1. GPL-2.0-or-later combined works. XenonAnalyse and XenonRecomp (usr/share/legacyrecomp/tools/) link the
   PowerPC disassembler from GNU binutils (thirdparty/XenonRecomp/thirdparty/disasm/), which is
   GPL-2.0-or-later. As combined works, these two programs are distributed under GPL-2.0-or-later. The
   disassembler is not part of legacyrecomp, legacyrecomp-gui or the game.

2. LGPL-2.1 static linking. legacyrecomp, legacyrecomp-gui, XenonAnalyse and XenonRecomp statically link
   lzxd.c from libmspack (LGPL-2.1), compiled into XenonUtils. The complete corresponding source code of
   this release, all submodules included, is published next to this AppImage as
   LegacyRecomp-$version-source.tar.gz. With it you can rebuild and relink these programs against a
   modified libmspack (build instructions: README.md, "Build from source", and docs/packaging.md).

3. The toolchain in usr/share/legacyrecomp/toolchain/ compiles the game on your machine. Zig and CMake are
   the unmodified upstream release binaries; Ninja and SDL2 are built unmodified from the upstream release
   sources (versions and SHA-256 in packaging/appimage/toolchain.lock inside the source archive). The zig
   directory is trimmed to the files needed for x86_64-linux-gnu; nothing in it is changed. Their sources:
   https://ziglang.org/download/, https://cmake.org/download/, https://github.com/ninja-build/ninja,
   https://github.com/libsdl-org/SDL.

4. The AppImage type2 runtime (https://github.com/AppImage/type2-runtime, release 20251108) is used
   unmodified. It statically links musl libc, libfuse 3.15.0 (LGPL-2.1, with the patch in that repository),
   squashfuse, libzstd, zlib and mimalloc (see its scripts/common/install-dependencies.sh). The
   corresponding source of the runtime and of libfuse, including that patch, is published next to this
   AppImage as LegacyRecomp-$version-runtime-sources.tar.gz.

5. XenonUtils (linked into legacyrecomp, legacyrecomp-gui, XenonAnalyse and XenonRecomp, and shipped as
   usr/share/legacyrecomp/runtime/lib/libXenonUtils.a) contains code from the Xenia project: the XEX patcher
   (xex_patcher.cpp, xex_patcher.h) and the kernel export tables (xbox/xboxkrnl_table.inc, xam_table.inc).
   That code is under Xenia's BSD-3-Clause license, reproduced below.

This file is an engineering reading of the licenses, not legal advice.
EOF

section "LegacyRecomp (MIT)" "$repo/LICENSE"
section "XenonRecomp, XenonAnalyse, XenonUtils (MIT)" "$repo/thirdparty/XenonRecomp/LICENSE.md"
printf '\n\nXenia (BSD-3-Clause), code in XenonUtils\n========================================\n\n'
printf -- '--- notice from thirdparty/XenonRecomp/XenonUtils/xex_patcher.cpp ---\n\n'
sed -n '3,10p' "$repo/thirdparty/XenonRecomp/XenonUtils/xex_patcher.cpp"
section "Xenia license (the LICENSE the notice above refers to)" "$here/licenses/xenia-LICENSE"
section "Dear ImGui (MIT)" "$repo/thirdparty/imgui/LICENSE.txt"
section "volk (MIT)" "$repo/thirdparty/volk/LICENSE.md"
section "Vulkan Memory Allocator (MIT)" "$repo/thirdparty/VulkanMemoryAllocator/LICENSE.txt"
section "Vulkan-Headers (Apache-2.0)" "$repo/thirdparty/Vulkan-Headers/LICENSES/Apache-2.0.txt"
section "toml++ (MIT)" "$tp/tomlplusplus/LICENSE"
section "{fmt} (MIT)" "$tp/fmt/LICENSE"
section "xxHash library (BSD-2-Clause)" "$tp/xxHash/LICENSE"
section "SIMD Everywhere (MIT)" "$tp/simde/COPYING"
section "tiny-AES-c (Unlicense)" "$tp/tiny-AES-c/unlicense.txt"
printf '\n\nTinySHA1 (ISC-style)\n====================\n\n--- notice from thirdparty/XenonRecomp/thirdparty/TinySHA1/TinySHA1.hpp ---\n\n'
header_comment "$tp/TinySHA1/TinySHA1.hpp" 24
printf '\n\nGNU binutils PowerPC disassembler (GPL-2.0-or-later)\n'
printf '====================================================\n\n'
printf -- '--- notice from thirdparty/XenonRecomp/thirdparty/disasm/ppc-dis.c ---\n\n'
header_comment "$tp/disasm/ppc-dis.c" 20
section "GNU General Public License, version 2 (binutils disassembler; Linux UAPI headers)" "$here/licenses/GPL-2.0.txt"
printf '\nLinux-syscall-note (applies to the Linux UAPI headers in toolchain/zig/lib/libc/include):\n\n'
cat <<'EOF'
   NOTE! This copyright does *not* cover user programs that use kernel
 services by normal system calls - this is merely considered normal use
 of the kernel, and does *not* fall under the heading of "derived work".
 Also note that the GPL below is copyrighted by the Free Software
 Foundation, but the instance of code that it refers to (the Linux
 kernel) is copyrighted by me and others who actually wrote it.
EOF
section "GNU Lesser General Public License, version 2.1 (libmspack; also glibc and libfuse)" \
  "$tp/libmspack/libmspack/COPYING.LIB"
section "Inter (SIL OFL 1.1)" "$repo/assets/fonts/LICENSE-Inter.txt"
section "JetBrains Mono (SIL OFL 1.1)" "$repo/assets/fonts/LICENSE-JetBrainsMono.txt"
section "SDL2 (zlib)" "$sdl2/LICENSE.txt"
section "Zig (MIT)" "$zig/LICENSE"
section "LLVM, Clang, LLD, libc++, libc++abi, libunwind, compiler-rt (Apache-2.0 WITH LLVM-exception)" \
  "$zig/lib/libcxx/LICENSE.TXT"
section "GNU C Library: additional copyright notices (LICENSES)" "$zig/lib/libc/glibc/LICENSES"
section "CMake (BSD-3-Clause)" "$cmake/doc/cmake/LICENSE.rst"
for d in "$cmake"/doc/cmake/cm*/; do
  name=$(basename "$d")
  # shellcheck disable=SC2046
  section "CMake bundled library: $name" $(find "$d" -type f | sort)
done
printf '\n\nOpenSSL 3.6.0 (Apache-2.0), statically linked in the official CMake binary\n'
printf '=========================================================================\n\n'
printf 'Copyright (c) 1998-2025 The OpenSSL Project Authors\nCopyright (c) 1995-1998 Eric A. Young, Tim J. Hudson\n'
printf 'All rights reserved.\n'
section "OpenSSL license text" "$here/licenses/openssl-LICENSE.txt"
section "Ninja (Apache-2.0)" "$ninja/COPYING"
section "AppImage type2 runtime (MIT)" "$here/licenses/appimage-type2-runtime-LICENSE.txt"
printf '\n\nlibfuse 3.15.0 (LGPL-2.1 for lib/ and include/), in the AppImage runtime\n'
printf '=======================================================================\n\n'
printf -- '--- notice from fuse-3.15.0/lib/fuse.c ---\n\n'
header_comment "$libfuse/lib/fuse.c" 10
section "libfuse license terms and authors (the LGPL-2.1 text is above)" "$libfuse/LICENSE" "$libfuse/AUTHORS"
section "musl libc (MIT), in the AppImage runtime" "$here/licenses/musl-COPYRIGHT.txt"
section "squashfuse (BSD-2-Clause), in the AppImage runtime" "$here/licenses/squashfuse-LICENSE.txt"
section "Zstandard (BSD-3-Clause), in the AppImage runtime" "$here/licenses/zstd-LICENSE.txt"
section "zlib (zlib), in the AppImage runtime" "$here/licenses/zlib-LICENSE.txt"
section "mimalloc (MIT), in the AppImage runtime" "$here/licenses/mimalloc-LICENSE.txt"
} > "$out"
