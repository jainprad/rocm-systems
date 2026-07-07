#!/usr/bin/env bash
#
# Phase 1 PR coverage gate.
#
# Given an already-built + already-run instrumented tree (i.e. *.gcda files
# exist next to the *.gcno files produced by a --coverage build), this script:
#
#   1. Collects coverage into a Cobertura XML report (gcovr) scoped to the
#      changed component.
#   2. Computes DIFF (patch) coverage: the percentage of the lines this PR
#      changed that were executed by the tests.
#   3. Fails (non-zero exit) if patch coverage is below the threshold, which
#      is what blocks the PR in CI.
#
# It intentionally gates on *diff* coverage, not total/project coverage, so a
# large legacy codebase does not dilute the signal for a small PR.
#
# Usage:
#   ci/coverage/collect_and_gate.sh
#
# Configuration (environment variables, with defaults):
#   BASE_REF     Branch/ref to diff against            (default: origin/develop)
#   COV_ROOT     Source subtree to measure             (default: projects/clr)
#   FAIL_UNDER   Minimum patch coverage %% to pass      (default: 80)
#   BUILD_DIR    Dir containing .gcda/.gcno files       (default: build)
#   REPORT_DIR   Output dir for reports                 (default: coverage-reports)
#   EXCLUDE_RE   Extra gcovr --exclude regex (optional)
#
set -euo pipefail

BASE_REF="${BASE_REF:-origin/develop}"
COV_ROOT="${COV_ROOT:-projects/clr}"
FAIL_UNDER="${FAIL_UNDER:-80}"
BUILD_DIR="${BUILD_DIR:-build}"
REPORT_DIR="${REPORT_DIR:-coverage-reports}"
EXCLUDE_RE="${EXCLUDE_RE:-}"

echo "== PR diff-coverage gate =="
echo "  base ref     : ${BASE_REF}"
echo "  coverage root: ${COV_ROOT}"
echo "  fail-under   : ${FAIL_UNDER}%"
echo "  build dir    : ${BUILD_DIR}"

# ---------------------------------------------------------------------------
# 0. Guard: coverage data must exist.
#    .gcno is produced at COMPILE time (--coverage) and .gcda at RUN time
#    (tests execute). If the instrumented build/tests did not run on this
#    runner (e.g. a hosted runner with no ROCm toolchain/GPU), there is
#    nothing to measure. Skip the gate neutrally instead of erroring; the
#    gate is enforced on a ROCm-capable runner where the build + tests run.
# ---------------------------------------------------------------------------
if [[ ! -d "${BUILD_DIR}" ]] || ! find "${BUILD_DIR}" -type f -name '*.gcda' 2>/dev/null | grep -q .; then
  echo "::warning title=Coverage skipped::No coverage data (*.gcda) under '${BUILD_DIR}'."
  echo "The instrumented CLR/HIP build+tests did not run on this runner"
  echo "(requires ROCm toolchain for compile and an AMD GPU to run the tests)."
  echo "Skipping the diff-coverage gate. Enforce it on a ROCm-capable runner."
  exit 0
fi

command -v gcovr     >/dev/null 2>&1 || { echo "ERROR: gcovr not found";     exit 2; }
command -v diff-cover >/dev/null 2>&1 || { echo "ERROR: diff-cover not found"; exit 2; }

mkdir -p "${REPORT_DIR}"

# ---------------------------------------------------------------------------
# 1. Collect coverage -> Cobertura XML (+ HTML for humans).
#    Tests and third-party code are excluded so they don't count as "covered
#    product code" and don't inflate/deflate the number.
# ---------------------------------------------------------------------------
GCOVR_ARGS=(
  --root .
  --filter "${COV_ROOT}/"
  --exclude '.*/tests?/.*'
  --exclude '.*/test/.*'
  --exclude '.*/third_party/.*'
  --exclude '.*/external/.*'
  --gcov-ignore-parse-errors
  --print-summary
)
[[ -n "${EXCLUDE_RE}" ]] && GCOVR_ARGS+=( --exclude "${EXCLUDE_RE}" )

echo "== Collecting coverage with gcovr =="
gcovr "${GCOVR_ARGS[@]}" \
  --cobertura "${REPORT_DIR}/coverage.xml" \
  --html-details "${REPORT_DIR}/coverage.html" \
  "${BUILD_DIR}" || {
    echo "ERROR: gcovr failed. Was the tree built with --coverage and were tests run?"
    exit 2
  }

# ---------------------------------------------------------------------------
# 2 + 3. Diff coverage gate. diff-cover intersects the report with the git
#        diff against the base branch and fails under the threshold.
# ---------------------------------------------------------------------------
echo "== Enforcing diff (patch) coverage >= ${FAIL_UNDER}% =="
set +e
diff-cover "${REPORT_DIR}/coverage.xml" \
  --compare-branch="${BASE_REF}" \
  --fail-under="${FAIL_UNDER}" \
  --format "html:${REPORT_DIR}/diff-coverage.html,markdown:${REPORT_DIR}/diff-coverage.md"
gate_rc=$?
set -e

# ---------------------------------------------------------------------------
# 4. Report where to view the HTML reports.
#    - coverage.html      : full annotated source coverage (gcovr)
#    - diff-coverage.html : the PR patch-coverage report (diff-cover)
#    In CI these are uploaded as the "coverage-reports" artifact; download and
#    open the HTML files in a browser.
# ---------------------------------------------------------------------------
report_abs="$(cd "${REPORT_DIR}" && pwd)"
echo "== Coverage HTML reports =="
echo "  full coverage : ${report_abs}/coverage.html"
echo "  patch coverage: ${report_abs}/diff-coverage.html"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  {
    echo "### Coverage reports"
    echo "Download the **coverage-reports** artifact from this run and open:"
    echo "- \`coverage.html\` — full annotated source coverage"
    echo "- \`diff-coverage.html\` — PR patch (diff) coverage"
  } >> "${GITHUB_STEP_SUMMARY}"
fi

if [[ ${gate_rc} -ne 0 ]]; then
  echo "::error::PR diff coverage is below ${FAIL_UNDER}%. Add tests that exercise the changed lines."
else
  echo "PR diff coverage gate PASSED."
fi
exit ${gate_rc}
