#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

export PKG_CONFIG_PATH="/opt/homebrew/opt/curl/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

BUILD="${BUILD:-Debug}"

python3 << PYEOF
import json, os, re, subprocess

cwd = os.getcwd()
result = subprocess.run(
    ["make", "BUILD=${BUILD}", "-Bn", "all"],
    capture_output=True, text=True,
    env=os.environ,
    cwd=cwd,
)
commands = []
seen = set()
for line in result.stdout.splitlines():
    m = re.match(r'^(c\+\+|cc)\s+-c\s+-o\s+(\S+)\s+(\S+)', line)
    if not m:
        continue
    _, _, src = m.groups()
    if src in seen:
        continue
    seen.add(src)
    commands.append({"directory": cwd, "file": src, "command": line})

with open("compile_commands.json", "w") as f:
    json.dump(commands, f, indent=2)

print(f"Wrote {len(commands)} entries to compile_commands.json (BUILD=${BUILD})")
PYEOF
