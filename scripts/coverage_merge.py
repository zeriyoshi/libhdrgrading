# SPDX-License-Identifier: Apache-2.0 OR MIT
# Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
"""Merges the platforms' coverage (scripts/coverage.sh's coverage.lcov) into one report.

    python3 -I scripts/coverage_merge.py OUT LABEL=coverage.lcov [LABEL=coverage.lcov ...]

Each input is `llvm-cov export -format=lcov` of one platform's tests. The paths are made relative to
the repository (from the first "/src/" or "/include/" in them whose rest is a file of the
repository, else the last, with either separator), so the
platforms' records of a file meet; the files outside src/ and include/ are left out. A line, a
function or a branch side counts as covered when it ran on any platform, and as one to cover when
any platform compiled it (a platform's own code, such as Metal's or Windows', counts once, where
it is built). OUT receives:

    merged.lcov     the union, for genhtml
    summary.md      per platform and the union: lines, functions, branch sides, and the lines no
                    other platform covered
    uncovered.md    the functions never called on any platform and the branches a side of which
                    never ran on any, by file and line (lcov has no columns: a line's branches are
                    numbered in their order on it)

The last line printed gives the union's totals.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATIC = re.compile(r"^(?:.*[/\\])?[^/\\:]+\.(?:c|h|m|mm|cc|cpp|cxx):(?=.)")


def relative(path):
    path = path.replace("\\", "/")
    found = sorted(m.start() for m in re.finditer(r"/(?=(?:src|include)/)", path))
    # the first whose rest is a file of the repository (src/foundation/include/... holds an
    # "/include/" too), else the last
    for at in found:
        if os.path.isfile(os.path.join(ROOT, path[at + 1:])):
            return path[at + 1:]
    if found:
        return path[found[-1] + 1:]
    return path if path.startswith(("src/", "include/")) else None


def read(path):
    """{file: {"lines": {line: count}, "functions": {name: [line, count]},
               "branches": {(line, block, branch): count}}}"""
    files = {}
    item = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if line.startswith("SF:"):
                name = relative(line[3:])
                item = None
                if name is not None:
                    item = files.setdefault(name, {"lines": {}, "functions": {}, "branches": {}})
            elif item is None or line == "end_of_record":
                item = None if line == "end_of_record" else item
            elif line.startswith("DA:"):
                fields = line[3:].split(",")
                number, count = int(fields[0]), int(fields[1])
                item["lines"][number] = item["lines"].get(number, 0) + count
            elif line.startswith("FN:"):
                start, name = line[3:].split(",", 1)
                name = STATIC.sub("", name)
                item["functions"].setdefault(name, [int(start), 0])
            elif line.startswith("FNDA:"):
                count, name = line[5:].split(",", 1)
                name = STATIC.sub("", name)
                item["functions"].setdefault(name, [0, 0])[1] += int(count)
            elif line.startswith("BRDA:"):
                number, block, branch, taken = line[5:].split(",")
                key = (int(number), int(block), int(branch))
                item["branches"][key] = item["branches"].get(key, 0) + (
                    0 if taken == "-" else int(taken))
    return files


def merge(platforms):
    union = {}
    for files in platforms.values():
        for name, item in files.items():
            into = union.setdefault(name, {"lines": {}, "functions": {}, "branches": {}})
            for number, count in item["lines"].items():
                into["lines"][number] = into["lines"].get(number, 0) + count
            for function, (start, count) in item["functions"].items():
                entry = into["functions"].setdefault(function, [start, 0])
                entry[0] = entry[0] or start
                entry[1] += count
            for key, count in item["branches"].items():
                into["branches"][key] = into["branches"].get(key, 0) + count
    return union


def totals(files):
    result = {"lines": [0, 0], "functions": [0, 0], "branches": [0, 0]}
    for item in files.values():
        for kind in result:
            values = item[kind].values()
            counts = [value[1] for value in values] if kind == "functions" else list(values)
            result[kind][0] += sum(1 for count in counts if count > 0)
            result[kind][1] += len(counts)
    return result


def percent(covered, total):
    return 100.0 * covered / total if total else 100.0


def write_lcov(union, path):
    out = []
    for name in sorted(union):
        item = union[name]
        out += ["TN:", f"SF:{name}"]
        functions = sorted(item["functions"].items(), key=lambda kv: (kv[1][0], kv[0]))
        out += [f"FN:{start},{function}" for function, (start, _) in functions]
        out += [f"FNDA:{count},{function}" for function, (_, count) in functions]
        out += [f"FNF:{len(functions)}", f"FNH:{sum(1 for _, (_, c) in functions if c > 0)}"]
        branches = sorted(item["branches"].items())
        out += [f"BRDA:{n},{b},{k},{c}" for (n, b, k), c in branches]
        out += [f"BRF:{len(branches)}", f"BRH:{sum(1 for _, c in branches if c > 0)}"]
        lines = sorted(item["lines"].items())
        out += [f"DA:{n},{c}" for n, c in lines]
        out += [f"LF:{len(lines)}", f"LH:{sum(1 for _, c in lines if c > 0)}", "end_of_record"]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def only_here(platforms):
    """the lines each platform covered that no other did"""
    covered = {
        label: {(name, n) for name, item in files.items() for n, c in item["lines"].items() if c > 0}
        for label, files in platforms.items()
    }
    result = {}
    for label, lines in covered.items():
        others = set().union(*(v for k, v in covered.items() if k != label)) if len(covered) > 1 else set()
        result[label] = len(lines - others)
    return result


def write_summary(platforms, union, path):
    rows = ["| Platform | Lines | Functions | Branch sides | Lines only here |",
            "|---|---:|---:|---:|---:|"]
    unique = only_here(platforms)

    def cells(t):
        return [f"{percent(*t[k]):.2f}% ({t[k][0]}/{t[k][1]})" for k in ("lines", "functions", "branches")]

    for label in sorted(platforms):
        rows.append("| " + " | ".join([label] + cells(totals(platforms[label])) + [str(unique[label])]) + " |")
    rows.append("| " + " | ".join(["**all (union)**"] + cells(totals(union)) + ["-"]) + " |")
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"## Coverage of src/ + include/ over {len(platforms)} platforms\n\n" + "\n".join(rows) +
                "\n\nCovered: run on any platform; to cover: built on any. The functions never "
                "called and the branches a side of which never ran: uncovered.md\n")


def write_uncovered(union, path):
    never = sorted((name, start, function) for name, item in union.items()
                   for function, (start, count) in item["functions"].items() if count == 0)
    sides = {}
    for name, item in union.items():
        pairs = {}
        for (number, block, branch), count in item["branches"].items():
            pairs.setdefault((number, block, branch // 2), [0, 0])[branch % 2] += count
        for (number, block, index), (true_count, false_count) in sorted(pairs.items()):
            if true_count == 0 and false_count == 0:
                what = "neither side"
            elif true_count == 0:
                what = "never true"
            elif false_count == 0:
                what = "never false"
            else:
                continue
            sides.setdefault(name, []).append((number, block, index, what))
    total = sum(len(rows) for rows in sides.values())
    out = ["# Uncovered code over every platform (src/ + include/)", "",
           f"## Functions never called ({len(never)})", ""]
    out += [f"- `{name}:{start}` {function}" for name, start, function in never]
    out += ["", f"## Branches a side of which never ran ({total})", ""]
    for name in sorted(sides):
        out += [f"### {name} ({len(sides[name])})", ""]
        out += [f"- {number} (branch {block}.{index}) {what}" for number, block, index, what in sides[name]]
        out.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    return len(never), total


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    out = argv[1]
    platforms = {}
    for arg in argv[2:]:
        label, _, path = arg.partition("=")
        if not path:
            print(f"error: {arg}: LABEL=coverage.lcov expected", file=sys.stderr)
            return 2
        platforms[label] = read(path)
    os.makedirs(out, exist_ok=True)
    union = merge(platforms)
    write_lcov(union, os.path.join(out, "merged.lcov"))
    write_summary(platforms, union, os.path.join(out, "summary.md"))
    never, sides = write_uncovered(union, os.path.join(out, "uncovered.md"))
    t = totals(union)
    print(f"{len(platforms)} platforms: lines {percent(*t['lines']):.2f}%, functions "
          f"{percent(*t['functions']):.2f}%, branch sides {percent(*t['branches']):.2f}%; never called: "
          f"{never} functions; a side never ran: {sides} branches")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
