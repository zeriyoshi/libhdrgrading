#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Builds a coverage preset (Clang source-based coverage: "coverage", or HDRG_COVERAGE_PRESET such as
# "coverage-msvc" for clang-cl on Windows), runs the tests, merges the profiles and writes reports
# for the project's own src/ and include/ (tests/ and third_party/ excluded):
#   build/[$HDRG_BUILD_PREFIX]<preset>/hdrg-coverage/report.txt      llvm-cov report (per file)
#   build/[$HDRG_BUILD_PREFIX]<preset>/hdrg-coverage/summary.md      Markdown totals (for $GITHUB_STEP_SUMMARY)
#   build/[$HDRG_BUILD_PREFIX]<preset>/hdrg-coverage/summary.json    llvm-cov export -summary-only
#   build/[$HDRG_BUILD_PREFIX]<preset>/hdrg-coverage/coverage.lcov   llvm-cov export -format=lcov (the
#                       platforms' are merged by scripts/coverage_merge.py)
#   build/[$HDRG_BUILD_PREFIX]<preset>/hdrg-coverage/html/index.html llvm-cov show -format=html
#   build/[$HDRG_BUILD_PREFIX]<preset>/hdrg-coverage/uncovered.md    the functions never called and the
#                       branches a side of which never ran (scripts/coverage_uncovered.py)
#
#   scripts/coverage.sh                 build, test and report
#   scripts/coverage.sh --keep-going    report even when tests fail, then exit with ctest's status
#   scripts/coverage.sh --report-only   report the profiles already in the build tree
#
# HDRG_COVERAGE_LABEL names the platform in summary.md; HDRG_PYTHON is the Python (default python3).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Git Bash on Windows: the native tools take D:/a/... (not /d/a/...)
if command -v cygpath >/dev/null 2>&1; then
    root="$(cygpath -m "$root")"
fi
preset="${HDRG_COVERAGE_PRESET:-coverage}"
build="$root/build/${HDRG_BUILD_PREFIX:-}$preset"
out="$build/hdrg-coverage"
mode="${1:-}"
python="${HDRG_PYTHON:-python3}"

find_llvm_tool() {
    local name="$1"

    if command -v xcrun >/dev/null 2>&1 && xcrun --find "$name" >/dev/null 2>&1; then
        xcrun --find "$name"

        return
    fi
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"

        return
    fi

    printf 'error: %s was not found\n' "$name" >&2

    exit 1
}

profdata="${HDRG_LLVM_PROFDATA:-$(find_llvm_tool llvm-profdata)}"
cov="${HDRG_LLVM_COV:-$(find_llvm_tool llvm-cov)}"

cd "$root"
tested=0
if [[ "$mode" != "--report-only" ]]; then
    rm -rf "$out/profiles"
    cmake --preset "$preset"
    cmake --build --preset "$preset"
    if [[ "$mode" == "--keep-going" ]]; then
        ctest --preset "$preset" || tested=$?
    else
        ctest --preset "$preset"
    fi
fi

"$profdata" merge -sparse "$out"/profiles/*.profraw -o "$out/merged.profdata"

objects=()
while IFS= read -r test_name; do
    # (a test of a script, such as the sidecar's, has no program: its name may be a directory)
    program="$build/$test_name"
    if [[ -f "$program.exe" ]]; then
        program="$program.exe"
    fi
    if [[ ! -f "$program" || ! -x "$program" ]]; then
        continue
    fi

    if [[ ${#objects[@]} -eq 0 ]]; then
        objects+=("$program")
    else
        objects+=(-object "$program")
    fi
done < <(ctest --test-dir "$build" -N | tr -d '\r' | sed -n 's/^ *Test *#[0-9]*: //p')

common=(-instr-profile="$out/merged.profdata" "-ignore-filename-regex=[/\\\\](tests|third_party|build)[/\\\\]")
sources=(-sources "$root/src" "$root/include")

"$cov" report "${common[@]}" "${objects[@]}" "${sources[@]}" > "$out/report.txt"
"$cov" export -format=text -summary-only "${common[@]}" "${objects[@]}" "${sources[@]}" \
    > "$out/summary.json"
"$cov" export -format=text -skip-expansions "${common[@]}" "${objects[@]}" "${sources[@]}" \
    > "$out/coverage.json"
"$cov" export -format=lcov "${common[@]}" "${objects[@]}" "${sources[@]}" > "$out/coverage.lcov"
rm -rf "$out/html"
"$cov" show -format=html -show-branches=count -output-dir="$out/html" "${common[@]}" \
    "${objects[@]}" "${sources[@]}"
"$python" -I "$root/scripts/coverage_uncovered.py" "$out/coverage.json" "$out/uncovered.md" "$root"
cp "$out/uncovered.md" "$out/html/uncovered.md"

"$python" -I - "$out/summary.json" "$out/summary.md" "${HDRG_COVERAGE_LABEL:-}" <<'EOF'
import json
import sys

totals = json.load(open(sys.argv[1], encoding="utf-8"))["data"][0]["totals"]
label = f", {sys.argv[3]}" if sys.argv[3] else ""
rows = ["| Metric | Covered | Total | % |", "|---|---:|---:|---:|"]
for key in ("lines", "functions", "regions", "branches"):
    item = totals[key]
    rows.append(f"| {key} | {item['covered']} | {item['count']} | {item['percent']:.2f} |")
with open(sys.argv[2], "w", encoding="utf-8") as f:
    f.write(f"## Coverage (src/ + include/{label})\n\n" + "\n".join(rows) + "\n\n"
            "The functions never called and the branches a side of which never ran: "
            "hdrg-coverage/uncovered.md\n")
EOF

tail -n 1 "$out/report.txt"
printf 'HTML: %s\n' "$out/html/index.html"
printf 'uncovered: %s\n' "$out/uncovered.md"
if [[ "$tested" != 0 ]]; then
    printf 'error: tests failed (ctest status %s); the reports cover the runs\n' "$tested" >&2
fi
exit "$tested"
