#!/usr/bin/env bash
# Replays a real ITCH day through replay_full_market and compares message,
# frame, applied, and drop counts against tests/golden/<MMDDYYYY>.txt.
# Counts only: no prices or market data are stored in the golden file.
#
# Usage: scripts/check_golden.sh [--update] [MMDDYYYY] [itch_dir] [build_dir]
# Exit: 0 match, 1 mismatch or error, 2 tape missing (skipped).
set -euo pipefail

update=0
if [[ "${1:-}" == "--update" ]]; then
  update=1
  shift
fi
day="${1:-12302019}"
itch_dir="${2:-/tmp/itch}"
build_dir="${3:-build-native}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
golden="${repo_root}/tests/golden/${day}.txt"
tape="${itch_dir}/${day}.NASDAQ_ITCH50.bin"
replay="${repo_root}/${build_dir}/replay_full_market"

if [[ ! -f "${tape}" ]]; then
  echo "skip: ${tape} not found" >&2
  exit 2
fi
if [[ ! -x "${replay}" ]]; then
  echo "error: ${replay} not built" >&2
  exit 1
fi

actual="$("${replay}" "${tape}" | grep -E '^(Total messages in file|Frames|Events applied across all symbols|Drops):' || true)"
if [[ -z "${actual}" ]]; then
  echo "error: replay printed no count lines" >&2
  exit 1
fi

if [[ "${update}" -eq 1 ]]; then
  mkdir -p "$(dirname "${golden}")"
  printf '%s\n' "${actual}" > "${golden}"
  echo "updated ${golden}"
  exit 0
fi

if [[ ! -f "${golden}" ]]; then
  echo "error: ${golden} missing; run with --update to create it" >&2
  exit 1
fi

if diff -u "${golden}" <(printf '%s\n' "${actual}"); then
  echo "golden ${day}: match"
else
  echo "golden ${day}: MISMATCH" >&2
  exit 1
fi
