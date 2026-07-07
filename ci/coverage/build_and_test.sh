#!/usr/bin/env bash
#
# Real ROCm coverage build+test for the graph (segment-scheduling) PR, meant to
# run on a self-hosted ROCm + AMD-GPU runner (gfx950 here). It:
#   1. builds the CLR HIP runtime (amdhip64) with gcov --coverage instrumentation
#   2. builds the hip-tests graph test binaries (GraphsTest1/2)
#   3. runs the graph tests that exercise this PR's changed code against the
#      instrumented libamdhip64.so, producing *.gcda for collect_and_gate.sh
#
# Output layout expected by collect_and_gate.sh: BUILD_DIR=build (CLR build with
# the .gcno/.gcda), COV_ROOT=projects/clr.
#
set -euo pipefail

export PATH=/opt/rocm/bin:$PATH
NPROC="$(nproc)"
COV_FLAGS="--coverage -O0 -g -fprofile-update=atomic"

if ! command -v hipcc >/dev/null 2>&1; then
  echo "::warning::ROCm toolchain not found; skipping real coverage build."
  exit 0
fi

echo "== Installing python build deps (user) =="
python3 -m pip install --user --quiet CppHeaderParser pyyaml 2>/dev/null || true

# ---------------------------------------------------------------------------
# 1. Build CLR (amdhip64) with coverage instrumentation.
# ---------------------------------------------------------------------------
echo "== Configuring + building CLR (amdhip64) with --coverage =="
cmake -S projects/clr -B build \
  -DHIP_COMMON_DIR="$PWD/projects/hip" \
  -DCMAKE_PREFIX_PATH=/opt/rocm \
  -DCLR_BUILD_HIP=ON -DCLR_BUILD_OCL=OFF -DHIP_PLATFORM=amd \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="${COV_FLAGS}" -DCMAKE_CXX_FLAGS="${COV_FLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS="--coverage" -DCMAKE_SHARED_LINKER_FLAGS="--coverage"
cmake --build build --target amdhip64 --parallel "${NPROC}"

BLIB="$PWD/build/hipamd/lib"

# ---------------------------------------------------------------------------
# 2. Build the hip-tests graph test binaries.
# ---------------------------------------------------------------------------
echo "== Configuring + building hip-tests graph binaries =="
cmake -S projects/hip-tests/catch -B htbuild \
  -DHIP_PLATFORM=amd -DCMAKE_PREFIX_PATH=/opt/rocm -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=amdclang++ -DCMAKE_C_COMPILER=amdclang \
  -DCMAKE_HIP_COMPILER=amdclang++ -DOFFLOAD_ARCH_STR="--offload-arch=gfx950"
cmake --build htbuild --target GraphsTest1 GraphsTest2 --parallel "${NPROC}"

# ---------------------------------------------------------------------------
# 3. Reset counters and run the PR-relevant graph tests against the
#    instrumented runtime. Targeted subsets are used so the processes exit
#    cleanly (gcov flushes .gcda only on normal exit).
# ---------------------------------------------------------------------------
find build -name '*.gcda' -delete 2>/dev/null || true
G1=htbuild/catch_tests/unit/graph/GraphsTest1
G2=htbuild/catch_tests/unit/graph/GraphsTest2
export HIP_VISIBLE_DEVICES=0
export LD_LIBRARY_PATH="${BLIB}:${LD_LIBRARY_PATH:-}"

echo "== Running graph tests (empty-node, instantiate, host-func) =="
"${G1}" "*Empty*,*InstantiateWithFlags*" || echo "::warning::some GraphsTest1 cases failed (coverage still collected)"
"${G2}" "*HostFunc*"                       || echo "::warning::some GraphsTest2 cases failed (coverage still collected)"

echo "== Build + test complete; .gcda ready under build/ =="
