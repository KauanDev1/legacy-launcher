#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Initialises the submodules a build needs. Not `git submodule update --init --recursive`: that would also fetch the
# nested submodules of thirdparty/XenosRecomp we never build (dxc-bin: prebuilt DXC binaries for every platform; fmt
# and xxHash: XenonRecomp's copies are used instead, see CMakeLists.txt).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
git submodule update --init
git -C thirdparty/XenosRecomp submodule update --init thirdparty/smol-v thirdparty/zstd
git -C thirdparty/XenonRecomp submodule update --init --recursive
