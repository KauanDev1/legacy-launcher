#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build, test and run LegacyRecomp inside a Linux container (see docs/development.md).
set -euo pipefail

IMAGE=legacyrecomp-dev
BUILD_VOLUME=lr-build
DATA_VOLUME=lr-data
PRESET=${LR_PRESET:-dev}

usage() {
  cat >&2 <<USAGE
usage: tools/dev/container.sh <command> [args]
  shell                     interactive shell in the container (sources synced, /work)
  build                     sync sources and build the '$PRESET' preset
  test [ctest args...]      build, then ctest --preset $PRESET (default: -L unit)
  install --iso <file>      build, then 'legacyrecomp install --iso ... --root /data'
  run [-- game args...]     build, then 'legacyrecomp run --root /data [game args]'
environment: LR_PRESET (default dev; release is also valid)
USAGE
  exit 2
}

[ $# -ge 1 ] || usage
cmd=$1
shift

# Git Bash on Windows rewrites paths in arguments; keep them untouched.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

if ! command -v docker >/dev/null 2>&1; then
  echo "container.sh: docker was not found in PATH" >&2
  exit 1
fi
if ! docker info >/dev/null 2>&1; then
  echo "container.sh: Docker is not running (start Docker Desktop or the docker daemon)" >&2
  exit 1
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
# Docker on Windows wants a drive path (D:/...), which pwd -W provides in Git Bash.
if repo_host=$(cd "$repo_root" && pwd -W 2>/dev/null); then :; else repo_host=$repo_root; fi

# Build the image (cached by Docker; rebuilt when the Dockerfile changes).
script_host=$repo_host/tools/dev
docker build -q -t "$IMAGE" -f "$script_host/Dockerfile" "$script_host" >/dev/null

mem_bytes=$(docker info --format '{{.MemTotal}}' 2>/dev/null || echo 0)
jobs=$(( mem_bytes / 2147483648 ))
[ "$jobs" -ge 1 ] || jobs=1
[ "$jobs" -le 64 ] || jobs=64

docker_args=(run --rm --init --cap-add=SYS_PTRACE
  -v "$repo_host:/src:ro" -v "$BUILD_VOLUME:/work" -v "$DATA_VOLUME:/data"
  -e "LR_JOBS=$jobs" -e "LR_PRESET=$PRESET")

# Runs inside the container: sync tracked files into /work (keeping /work/build), then configure+build.
sync_script='
set -euo pipefail
git config --global --add safe.directory "*"
stage=$(mktemp -d)
cd /src
rc=0
git ls-files --recurse-submodules -z | tar --null -T - -cf - 2>/dev/null | (cd "$stage" && tar xf -) || rc=$?
cd /work
rsync -a --delete --exclude=/build "$stage"/ /work/
rm -rf "$stage"
'
build_script="$sync_script"'
cmake --preset "$LR_PRESET" >/dev/null
cmake --build --preset "$LR_PRESET" -j "$LR_JOBS"
'

run_in_container() { # <tty flags> <script> [args...]
  local tty=$1 script=$2
  shift 2
  # shellcheck disable=SC2086
  docker "${docker_args[@]:0:1}" $tty "${docker_args[@]:1}" "${extra_mounts[@]}" "$IMAGE" bash -c "$script" lr "$@"
}

extra_mounts=()
tty_flags=""
[ -t 0 ] && [ -t 1 ] && tty_flags="-it"

case "$cmd" in
  shell)
    run_in_container "$tty_flags" "$sync_script"'exec bash'
    ;;
  build)
    run_in_container "$tty_flags" "$build_script"
    ;;
  test)
    [ $# -ge 1 ] || set -- -L unit
    run_in_container "$tty_flags" "$build_script"'ctest --preset "$LR_PRESET" --output-on-failure "$@"' "$@"
    ;;
  install)
    iso=""
    rest=()
    while [ $# -gt 0 ]; do
      if [ "$1" = "--iso" ]; then
        [ $# -ge 2 ] || { echo "container.sh: --iso needs a file" >&2; exit 2; }
        iso=$2
        shift 2
      else
        rest+=("$1")
        shift
      fi
    done
    [ -n "$iso" ] || { echo "container.sh: install needs --iso <host path to the game disc image>" >&2; usage; }
    [ -f "$iso" ] || { echo "container.sh: ISO not found: $iso" >&2; exit 2; }
    iso_dir=$(cd "$(dirname "$iso")" && { pwd -W 2>/dev/null || pwd; })
    extra_mounts=(-v "$iso_dir:/iso:ro")
    run_in_container "$tty_flags" "$build_script"'exec "build/$LR_PRESET/bin/legacyrecomp" install --root /data "$@"' \
      --iso "/iso/$(basename "$iso")" "${rest[@]}"
    ;;
  run)
    [ "${1:-}" != "--" ] || shift
    run_in_container "$tty_flags" "$build_script"'exec "build/$LR_PRESET/bin/legacyrecomp" run --root /data "$@"' "$@"
    ;;
  *)
    usage
    ;;
esac
