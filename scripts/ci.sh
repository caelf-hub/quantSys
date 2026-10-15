#!/usr/bin/env bash
# The same checks locally and in GitHub Actions: sanitized build, every
# ctest target, then the portable Docker image.
#
# Env: QUANT_CI_BUILD_DIR (default build-ci), QUANT_CI_SKIP_DOCKER=1 to skip
# the image when docker is not on PATH (it is reported, not hidden).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
build_dir="${QUANT_CI_BUILD_DIR:-build-ci}"
jobs="$(nproc 2>/dev/null || echo 2)"

echo "== configure (ASan + UBSan) =="
cmake -S . -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DQUANT_SANITIZE=ON
echo "== build =="
cmake --build "${build_dir}" -j"${jobs}"
echo "== ctest =="
ctest --test-dir "${build_dir}" --output-on-failure

echo "== docker build =="
if [[ "${QUANT_CI_SKIP_DOCKER:-0}" == "1" ]]; then
  echo "SKIPPED: QUANT_CI_SKIP_DOCKER=1"
elif ! command -v docker >/dev/null 2>&1; then
  echo "error: docker not on PATH; set QUANT_CI_SKIP_DOCKER=1 to skip explicitly" >&2
  exit 1
else
  docker build -t quantsys:ci .
  docker run --rm quantsys:ci
fi
echo "== ci.sh passed =="
