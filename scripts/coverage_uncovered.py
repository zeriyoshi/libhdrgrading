# SPDX-License-Identifier: Apache-2.0 OR MIT
# Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
"""The functions never called and the branches a side of which never ran, from an llvm-cov export.

    python3 -I scripts/coverage_uncovered.py coverage.json uncovered.md ROOT

coverage.json is `llvm-cov export -format=text` of the merged profile; uncovered.md lists, with
paths relative to ROOT, every function nothing called (a static or inline one counts once: its
copies in every translation unit and object, named "unit.c:name", are one function, called when
any copy is) and every branch whose true or false side (or both) never ran in any of its copies,
by file and line. The last line printed gives the counts.
"""

import json
import os
import sys


def main(argv):
    data = json.load(open(argv[1], encoding="utf-8"))["data"][0]
    root = argv[3]

    def where(path):
        return os.path.relpath(path, root).replace(os.sep, "/")

    def ours(path):
        return path.startswith(("src/", "include/"))

    calls = {}
    for function in data["functions"]:
        region = function["regions"][0]
        name = function["name"].rsplit(":", 1)[-1]
        key = (where(function["filenames"][region[5]]), region[0], name)
        if ours(key[0]):
            calls[key] = max(calls.get(key, 0), function["count"])

    never = sorted(key for key, count in calls.items() if count == 0)

    # a branch's sides summed over its copies (a header's inline function in every unit, a macro
    # at every use): it ran when any copy did
    counts = {}
    for item in data["files"]:
        path = where(item["filename"])
        if not ours(path):
            continue
        for branch in item.get("branches", []):
            key = (path, branch[0], branch[1])
            summed = counts.setdefault(key, [0, 0])
            summed[0] += branch[4]
            summed[1] += branch[5]

    sides = {}
    for (path, line, column), (true_count, false_count) in counts.items():
        if true_count == 0 and false_count == 0:
            what = "neither side"
        elif true_count == 0:
            what = "never true"
        elif false_count == 0:
            what = "never false"
        else:
            continue
        sides.setdefault(path, []).append((line, column, what))

    for path in sides:
        sides[path].sort()

    total = sum(len(rows) for rows in sides.values())
    out = ["# Uncovered code (src/ + include/)", "", f"## Functions never called ({len(never)})", ""]
    out += [f"- `{path}:{line}` {name}" for path, line, name in never]
    out += ["", f"## Branches a side of which never ran ({total})", ""]
    for path in sorted(sides):
        out += [f"### {path} ({len(sides[path])})", ""]
        out += [f"- {line}:{column} {what}" for line, column, what in sides[path]]
        out.append("")

    with open(argv[2], "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

    print(f"never called: {len(never)} functions; a side never ran: {total} branches")


if __name__ == "__main__":
    main(sys.argv)
