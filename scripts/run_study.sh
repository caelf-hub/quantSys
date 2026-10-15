#!/usr/bin/env bash
# Fetches each day, runs the one-symbol study for each ticker into
# data/<day>/, then runs the held-out-day evaluator per ticker into
# data/eval/. Historical replay only; nothing here sends an order.
#
# Env: DAYS, TICKERS (space separated), BUILD_DIR (default build-native),
#      ITCH_DIR (default /tmp/itch), STUDY_ARGS (e.g. "--maker-rebate 0.002"),
#      SKIP_FETCH=1 to use only tapes already present.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

# Ten ITCH 5.0 days that still have a downloadable gzip on the Nasdaq
# sample directory (several 2018 names are checksum-only). Includes the
# two tapes already studied (07302019, 12302019).
days="${DAYS:-01302019 03272019 07302019 08302019 10182019 10302019 12302019 01302020 07132021 08132021}"
tickers="${TICKERS:-AAPL MSFT AMD INTC QQQ}"
build_dir="${BUILD_DIR:-build-native}"
itch_dir="${ITCH_DIR:-/tmp/itch}"
study="${build_dir}/research_imbalance"
evaluate="${build_dir}/research_evaluate"
read -r -a study_args <<< "${STUDY_ARGS:-}"

for tool in "${study}" "${evaluate}"; do
  if [[ ! -x "${tool}" ]]; then
    echo "error: ${tool} not built" >&2
    exit 1
  fi
done

failed=()
for day in ${days}; do
  missing=0
  for ticker in ${tickers}; do
    [[ -s "data/${day}/${ticker}_features.csv" ]] || missing=1
  done
  if [[ "${missing}" -eq 0 ]]; then
    echo "skip day (all tickers present): ${day}"
    continue
  fi
  if [[ "${SKIP_FETCH:-0}" != "1" ]]; then
    ITCH_DIR="${itch_dir}" ITCH_GZ_DIR="${ITCH_GZ_DIR:-${itch_dir}/gz}" scripts/fetch_itch_day.sh "${day}"
  fi
  tape="${itch_dir}/${day}.NASDAQ_ITCH50.bin"
  if [[ ! -f "${tape}" ]]; then
    echo "missing tape ${tape}" >&2
    failed+=("${day}")
    continue
  fi
  mkdir -p "data/${day}"
  for ticker in ${tickers}; do
    echo "study: ${day} ${ticker}"
    if ! "${study}" "${tape}" "${ticker}" "data/${day}/${ticker}_imbalance.csv" "${study_args[@]}" \
         > "data/${day}/${ticker}_report.txt" 2> "data/${day}/${ticker}_report.err"; then
      echo "study failed: ${day} ${ticker} (see data/${day}/${ticker}_report.err)" >&2
      failed+=("${day}/${ticker}")
    fi
  done
done

mkdir -p data/eval
for ticker in ${tickers}; do
  inputs=()
  for day in ${days}; do
    csv="data/${day}/${ticker}_features.csv"
    [[ -f "${csv}" ]] && inputs+=("${csv}")
  done
  if (( ${#inputs[@]} < 3 )); then
    echo "skip evaluate ${ticker}: ${#inputs[@]} days (need 3)" >&2
    continue
  fi
  echo "evaluate: ${ticker} on ${#inputs[@]} days"
  "${evaluate}" --ticker "${ticker}" --out data/eval "${inputs[@]}" | tee "data/eval/${ticker}_summary.txt"
done

if (( ${#failed[@]} > 0 )); then
  echo "failures: ${failed[*]}" >&2
  exit 1
fi
