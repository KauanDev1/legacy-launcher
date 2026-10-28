# SPDX-License-Identifier: MIT
# Validates every module of the runtime shader cache with spirv-val and prints counts only.
# Usage (inside the dev container): tools/shader_cache_stats.sh [cache dir]   (default /data/work/shaders/runtime)
# Each cache file is a 48-byte LRSV header, the microcode, then the SPIR-V; the SPIR-V word count sits at offset 32,
# so the module is the file's last 4*count bytes. Nothing from the files is printed.
set -euo pipefail

dir=${1:-/data/work/shaders/runtime}
[ -d "$dir" ] || { echo "shader_cache_stats: no such directory: $dir" >&2; exit 2; }
command -v spirv-val >/dev/null || { echo "shader_cache_stats: spirv-val not found" >&2; exit 2; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

files=0 valid=0 invalid=0 bad_header=0
vs=0 ps=0 vs_invalid=0 ps_invalid=0
declare -A versions=()
shopt -s nullglob
for f in "$dir"/*.spv; do
  case "$f" in *.raw.spv) continue ;; esac
  files=$((files + 1))
  name=$(basename "$f")
  version=${name%%-*}
  versions[$version]=$(( ${versions[$version]:-0} + 1 ))
  stage=unknown
  case "$name" in *-vs-*) stage=vs; vs=$((vs + 1)) ;; *-ps-*) stage=ps; ps=$((ps + 1)) ;; esac
  magic=$(head -c 4 "$f")
  words=$(od -An -t u4 -j 32 -N 4 "$f" | tr -d ' ')
  size=$(stat -c %s "$f")
  if [ "$magic" != "LRSV" ] || [ -z "$words" ] || [ $((words * 4 + 48)) -gt "$size" ]; then
    bad_header=$((bad_header + 1))
    invalid=$((invalid + 1))
    continue
  fi
  tail -c $((words * 4)) "$f" > "$tmp/m.spv"
  if spirv-val --target-env vulkan1.2 "$tmp/m.spv" >/dev/null 2>&1; then
    valid=$((valid + 1))
  else
    invalid=$((invalid + 1))
    case "$stage" in vs) vs_invalid=$((vs_invalid + 1)) ;; ps) ps_invalid=$((ps_invalid + 1)) ;; esac
  fi
done

echo "files=$files valid=$valid invalid=$invalid bad_header=$bad_header"
echo "vs=$vs vs_invalid=$vs_invalid ps=$ps ps_invalid=$ps_invalid"
for v in "${!versions[@]}"; do echo "version $v: ${versions[$v]}"; done
[ "$invalid" -eq 0 ]
