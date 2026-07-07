# PR Coverage Gate (Phase 1)

This directory implements a **per-PR diff (patch) coverage gate**: every pull
request is measured on the lines it changes, and the check fails when those
changed lines are not adequately exercised by tests (existing **or** newly
added in the PR). Wire it into branch protection to block under-tested PRs.

## Why diff coverage (not total coverage)

- **Total/project coverage** is dominated by legacy code and barely moves for a
  small PR — a poor gating signal.
- **Diff/patch coverage** measures only the PR's changed lines — exactly the
  "are these changes tested?" question we want to enforce.

## Components

| File | Role |
|------|------|
| `build_and_test.sh` | Build the changed component with coverage instrumentation (`--coverage`) and run the **full** test suite, producing `*.gcda` files. |
| `collect_and_gate.sh` | Collect coverage with `gcovr` → Cobertura XML, then run `diff-cover` to compute patch coverage and fail under the threshold. |
| `../../.github/workflows/pr-diff-coverage.yml` | CI workflow that runs the two scripts on every PR touching `projects/clr/**`. |

## How it works

```
PR → git diff vs base branch → changed lines
                                     │
build with --coverage → run tests → *.gcda
                                     │
gcovr → coverage.xml (Cobertura)     │
                                     ▼
        diff-cover(coverage.xml, --compare-branch=base, --fail-under=80)
                                     │
                    pass ── merge allowed
                    fail ── check red → merge blocked
```

Running the **full** suite is intentional: existing tests frequently already
cover a PR's changed lines, so a fix with no new test can still legitimately
pass. Only running the PR's new tests would undercount coverage and reject valid
changes.

## Configuration

Both scripts read environment variables (see headers). Common ones:

| Var | Default | Meaning |
|-----|---------|---------|
| `BASE_REF` | `origin/develop` | Branch to diff against |
| `COV_ROOT` | `projects/clr` | Source subtree to measure |
| `FAIL_UNDER` | `80` | Minimum patch-coverage % to pass |
| `BUILD_DIR` | `build` | Location of `.gcda`/`.gcno` files |

## Toolchain note

CLR/HIP requires the ROCm toolchain, and the full test suite needs AMD GPUs, so
the **build + test** step is meant for a ROCm-capable / self-hosted runner
(`runs-on: [self-hosted, linux, gpu, rocm]`). On a stock hosted runner the build
step detects the missing toolchain and skips gracefully; the **gate** step is
fully functional against any `*.gcda` produced by a `--coverage` build.

For a Clang/LLVM build, swap `gcovr` collection for
`llvm-profdata merge` + `llvm-cov export --format=lcov`; `diff-cover` accepts the
lcov report unchanged.

## Roadmap

- **Phase 1 (this):** full suite + diff-cover gate — simple and always sound.
- **Phase 2:** Change-Based Testing (Test Impact Analysis) to run only the tests
  impacted by the change, for speed. Selection is an optimization only; the
  gated number stays identical to a full-suite run.
- **Always:** a scheduled full-suite + full-coverage run on `develop` as a
  safety net for any selection miss.
