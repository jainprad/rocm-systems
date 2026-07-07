#!/usr/bin/env bash
#
# Phase 1 helper: build the changed component WITH coverage instrumentation and
# run its test suite so that *.gcda files are produced for collect_and_gate.sh.
#
# NOTE ON SCOPE:
#   The CLR/HIP runtime requires the ROCm toolchain (and, for the full test
#   suite, AMD GPU hardware). This step is therefore expected to run on a
#   ROCm-capable / self-hosted runner. On a stock hosted runner it will detect
#   the missing toolchain and exit with a clear message; the gate step can
#   still be exercised against a pre-produced coverage report.
#
# Wire your real build here (or override CONFIGURE_CMD / BUILD_CMD / TEST_CMD).
#
# Configuration (environment variables, with defaults):
#   SRC_DIR       CMake source dir           (default: projects/clr)
#   BUILD_DIR     CMake build dir            (default: build)
#   COV_FLAGS     Coverage compile/link flag (default: --coverage -O0 -g)
#   CONFIGURE_CMD Override configure step    (optional)
#   BUILD_CMD     Override build step        (optional)
#   TEST_CMD      Override test step         (optional)
#
set -euo pipefail

SRC_DIR="${SRC_DIR:-projects/clr}"
BUILD_DIR="${BUILD_DIR:-build}"
COV_FLAGS="${COV_FLAGS:---coverage -O0 -g}"

echo "== Phase 1 coverage build =="
echo "  src dir   : ${SRC_DIR}"
echo "  build dir : ${BUILD_DIR}"
echo "  cov flags : ${COV_FLAGS}"

if ! command -v cmake >/dev/null 2>&1; then
  echo "::warning::cmake not found - skipping build. Run this on a ROCm-capable runner."
  exit 0
fi
if ! command -v hipcc >/dev/null 2>&1 && [[ -z "${CONFIGURE_CMD:-}" ]]; then
  echo "::warning::ROCm/hipcc toolchain not detected - skipping CLR build on this runner."
  echo "           Provide CONFIGURE_CMD/BUILD_CMD/TEST_CMD or use a self-hosted ROCm runner."
  exit 0
fi

# --- Reset any stale coverage counters so numbers reflect only this run. ------
find "${BUILD_DIR}" -name '*.gcda' -delete 2>/dev/null || true

# --- Configure with coverage instrumentation. --------------------------------
if [[ -n "${CONFIGURE_CMD:-}" ]]; then
  eval "${CONFIGURE_CMD}"
else
  cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="${COV_FLAGS}" \
    -DCMAKE_CXX_FLAGS="${COV_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
    -DCMAKE_SHARED_LINKER_FLAGS="--coverage"
fi

# --- Build. ------------------------------------------------------------------
if [[ -n "${BUILD_CMD:-}" ]]; then
  eval "${BUILD_CMD}"
else
  cmake --build "${BUILD_DIR}" --parallel "$(nproc)"
fi

# --- Run the FULL test suite (existing + newly added tests). -----------------
# Running everything is intentional: existing tests frequently already cover a
# PR's changed lines. (CBT test-selection is a Phase 2 optimization layered on
# top of this same gate.)
if [[ -n "${TEST_CMD:-}" ]]; then
  eval "${TEST_CMD}"
else
  ctest --test-dir "${BUILD_DIR}" --output-on-failure || {
    echo "::warning::ctest reported failures; coverage will still be collected."
  }
fi

echo "== Build + test complete; .gcda files ready for collection. =="
