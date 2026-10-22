#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Initialises the submodules a build needs. Not `git submodule update --init --recursive`: XenonRecomp is the only
# submodule whose nested submodules we use (libmspack, fmt, xxHash, ...), so it is initialised recursively below.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
git submodule update --init
git -C thirdparty/XenonRecomp submodule update --init --recursive
