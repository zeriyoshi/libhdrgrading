#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Claude Code PostToolUse hook (.claude/settings.json): runs scripts/check_style.py on the C file an
# Edit / Write just changed. Violations go to stderr with exit status 2, which Claude Code feeds
# back to the model so it fixes them in the same turn.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

file="$(python3 -I -c '
import json, sys
data = json.load(sys.stdin)
print((data.get("tool_input") or {}).get("file_path") or (data.get("tool_response") or {}).get("filePath") or "")
')"

case "$file" in
    "$root"/src/*.[ch] | "$root"/include/*.[ch] | "$root"/tools/*.[ch] | "$root"/tests/*.[ch]) ;;
    *) exit 0 ;;
esac

if ! report="$(python3 -I "$root/scripts/check_style.py" "$file" 2>&1)"; then
    printf '%s\n%s\n' "$report" \
        "Fix these to follow CLAUDE.md コーディング規約 (declaration order/merging, blank lines between logic blocks)." >&2

    exit 2
fi
