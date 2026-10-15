#!/usr/bin/env bash
# Downloads one NASDAQ TotalView-ITCH 5.0 day from the public Nasdaq sample
# directory, verifies the gzip, and decompresses it for replay.
#
# Usage: scripts/fetch_itch_day.sh <MMDDYYYY>
# Env:   ITCH_DIR (default /tmp/itch) for <day>.NASDAQ_ITCH50.bin
#        ITCH_GZ_DIR (default $ITCH_DIR/gz) for the downloaded .gz
#        ITCH_MIN_FREE_GB (default 30) free space required before starting
# Market data stays out of the repo. Do not commit these files.
set -euo pipefail

base_url="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
day="${1:-}"

if [[ "${day}" == S0* ]]; then
  echo "error: ${day} looks like an ITCH v2 (S0*) file; only ITCH 5.0 is supported" >&2
  exit 1
fi
if [[ ! "${day}" =~ ^[0-9]{8}$ ]]; then
  echo "usage: $0 <MMDDYYYY>" >&2
  exit 1
fi

itch_dir="${ITCH_DIR:-/tmp/itch}"
gz_dir="${ITCH_GZ_DIR:-${itch_dir}/gz}"
min_free_gb="${ITCH_MIN_FREE_GB:-30}"
name="${day}.NASDAQ_ITCH50"
bin="${itch_dir}/${name}.bin"
gz="${gz_dir}/${name}.gz"
mkdir -p "${itch_dir}" "${gz_dir}"

if [[ -s "${bin}" ]]; then
  echo "skip: ${bin} already present"
  exit 0
fi

free_gb() { df --output=avail -BG "$1" | tail -1 | tr -dc '0-9'; }
for dir in "${itch_dir}" "${gz_dir}"; do
  if (( $(free_gb "${dir}") < min_free_gb )); then
    echo "error: less than ${min_free_gb} GB free in ${dir}" >&2
    exit 1
  fi
done

# Classic name first. Some later days are published as SMMDDYY-v50.txt.gz.
s_name="S${day:0:4}${day:6:2}-v50.txt.gz"
download_named() {
  local remote="$1"
  rm -f "${gz}.partial"
  curl --fail --location --silent --show-error \
       --retry 5 --retry-delay 15 \
       --output "${gz}.partial" "${base_url}/${remote}" || return 1
  mv "${gz}.partial" "${gz}"
}

if [[ ! -s "${gz}" ]]; then
  echo "download: ${name}.gz"
  if ! download_named "${name}.gz"; then
    echo "classic name missing; trying ${s_name}"
    rm -f "${gz}.partial"
    download_named "${s_name}"
  fi
fi
if ! gzip -t "${gz}"; then
  echo "gzip -t failed; downloading again" >&2
  rm -f "${gz}" "${gz}.partial"
  if ! download_named "${name}.gz"; then
    rm -f "${gz}.partial"
    download_named "${s_name}"
  fi
  gzip -t "${gz}"
fi

decompress() { gzip -dc "${gz}" > "${bin}.partial" && mv "${bin}.partial" "${bin}"; }
echo "decompress: ${bin}"
if ! decompress; then
  # The July 30 decompress once failed with "Cannot allocate memory".
  echo "decompress failed; retrying once" >&2
  rm -f "${bin}.partial"
  sleep 10
  decompress
fi
echo "ok: ${bin} ($(stat -c %s "${bin}") bytes)"
