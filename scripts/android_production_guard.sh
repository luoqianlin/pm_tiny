#!/usr/bin/env bash

capture_production_state() {
    local output=$1
    local raw_output="${output%.json}.txt"
    "${ADB[@]}" shell "/vendor/bin/pm2 list --json" 2>/dev/null | tr -d '\r' > "$raw_output"
    python3 - "$raw_output" "$output" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    text = stream.read()
try:
    data = json.loads(text)
    processes = data.get("processes")
    if not isinstance(processes, list):
        raise ValueError("missing process array")
    state = {item["name"]: item["state"] == "online" for item in processes}
except (json.JSONDecodeError, KeyError, TypeError, ValueError):
    state = {}
    for line in text.splitlines():
        if not re.match(r"^\|\s*\d+\s*\|", line):
            continue
        fields = [field.strip() for field in line.strip().strip("|").split("|")]
        if len(fields) < 6:
            raise SystemExit("cannot parse production process table")
        state[fields[1]] = fields[5] == "online"
    total = re.search(r"^Total:\s*(\d+)\s*$", text, re.MULTILINE)
    if total is None or int(total.group(1)) != len(state):
        raise SystemExit("cannot parse production process list")
with open(sys.argv[2], "w", encoding="utf-8") as stream:
    json.dump(state, stream, ensure_ascii=False, sort_keys=True, indent=2)
    stream.write("\n")
PY
}

assert_production_unchanged() {
    local before=$1 after=$2
    python3 - "$before" "$after" <<'PY'
import json
import sys

def load(path):
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)

before = load(sys.argv[1])
after = load(sys.argv[2])
if before.keys() != after.keys():
    missing = sorted(before.keys() - after.keys())
    added = sorted(after.keys() - before.keys())
    raise SystemExit(f"production process set changed: missing={missing}, added={added}")
changed = sorted(name for name in before if before[name] != after[name])
if changed:
    raise SystemExit(f"production process online status changed: {changed}")
PY
}
