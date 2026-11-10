#!/bin/bash
#
# e2e.sh - end to end test of the skillctl workflow against a throwaway home.
#
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${SKILLCTL_BIN:-$ROOT/build/skillctl}"
WORK="${TMPDIR:-/tmp}/skillctl-e2e-$$"
FIX="$WORK/home"
STORE="$WORK/store"

pass=0
fail=0

ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
no() { fail=$((fail + 1)); printf '  FAIL %s\n' "$1"; }

check() { # check <description> <condition-as-shell>
    if eval "$2"; then ok "$1"; else no "$1"; fi
}

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

if [ ! -x "$BIN" ]; then
    echo "build the binary first: make"
    exit 1
fi

rm -rf "$WORK"
mkdir -p "$FIX"

# ------------------------------------------------------------- fixture --
mkdir -p "$FIX/.zcode/skills/alpha/references" "$FIX/.zcode/skills/beta"
printf -- '---\nname: alpha\ndescription: The alpha skill\n---\n\nbody\n' \
    > "$FIX/.zcode/skills/alpha/SKILL.md"
echo "reference" > "$FIX/.zcode/skills/alpha/references/note.md"
printf -- '---\nname: beta\ndescription: >\n  The beta skill\n  spanning lines\n---\n' \
    > "$FIX/.zcode/skills/beta/SKILL.md"

mkdir -p "$FIX/.claude/skills/alpha" "$FIX/.claude/skills/gamma"
printf -- '---\nname: alpha\ndescription: A different alpha\n---\n' \
    > "$FIX/.claude/skills/alpha/SKILL.md"
printf -- '---\nname: gamma\ndescription: Gamma\n---\n' \
    > "$FIX/.claude/skills/gamma/SKILL.md"

# a skill nested inside a skill must not count twice
mkdir -p "$FIX/.agents/skills/super/inner"
printf -- '---\nname: super\ndescription: Super skill\n---\n' > "$FIX/.agents/skills/super/SKILL.md"
printf -- '---\nname: inner\ndescription: Inner skill\n---\n' \
    > "$FIX/.agents/skills/super/inner/SKILL.md"

# the plugin cache layout: skills/<group>/<skill>
mkdir -p "$FIX/.zcode/cli/plugins/cache/plug/1.0.0/skills/godot/godot-2d"
printf -- '---\nname: godot-2d\ndescription: 2D in Godot\n---\n' \
    > "$FIX/.zcode/cli/plugins/cache/plug/1.0.0/skills/godot/godot-2d/SKILL.md"

# CRLF line endings and a quoted name
mkdir -p "$FIX/.cursor/skills/crlf"
printf -- '---\r\nname: "quoted-name"\r\ndescription: crlf file\r\n---\r\n' \
    > "$FIX/.cursor/skills/crlf/SKILL.md"

export HOME="$FIX"
export SKILLCTL_HOME="$STORE"

# ---------------------------------------------------------------- scan --
echo "scan"
out=$("$BIN" scan --json)
units=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin)["stats"]["units"])')
check "finds 7 skill directories" "[ '$units' = 7 ]"

names=$(echo "$out" | python3 -c '
import json,sys
d=json.load(sys.stdin)
print(" ".join(sorted(u["name"] for u in d["units_detail"])))')
check "names parsed correctly" \
    "[ '$names' = 'alpha alpha beta gamma godot-2d quoted-name super' ]"

roots=$(echo "$out" | python3 -c '
import json,sys
d=json.load(sys.stdin)
print(" ".join(sorted(r["path"] for r in d["skill_roots_detail"])))')
check "nested skill is not a separate unit" \
    "! echo '$roots' | grep -q 'super/inner' "

# -------------------------------------------------------------- import --
echo "import"
out=$("$BIN" import --json)
imported=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin)["imported"])')
check "imports every skill" "[ '$imported' = 7 ]"
check "the store holds one directory per skill" "[ \"\$(ls '$STORE/units' | wc -l | tr -d ' ')\" = 7 ]"

# a second import changes nothing
out=$("$BIN" import --json)
present=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin)["already_present"])')
check "a second import is a no-op" "[ '$present' = 7 ]"

# ---------------------------------------------------------------- link --
echo "link"
out=$("$BIN" link --json)
links=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin)["skills"])')
check "seven unique skills are linked" "[ '$links' = 7 ]"

for agent in zcode agents claude cursor; do
    check "$agent's skills directory is a symlink" "[ -L '$FIX/.$agent/skills' ]"
done

check "the zcode view is reachable through the symlink" \
    "[ -f '$FIX/.zcode/skills/alpha/SKILL.md' ]"
check "the claude view is reachable through the symlink" \
    "[ -f '$FIX/.claude/skills/gamma/SKILL.md' ]"
check "the quoted name survived the frontmatter parser" \
    "[ -f '$FIX/.cursor/skills/quoted-name/SKILL.md' ]"
check "the shared .agents skill reaches every agent" \
    "[ -f '$FIX/.zcode/skills/super/SKILL.md' ] && [ -f '$FIX/.cursor/skills/super/SKILL.md' ]"
check "the plugin cache skill is reachable" \
    "[ -f '$FIX/.zcode/skills/godot-2d/SKILL.md' ]"

# scope "own": each agent keeps its own version of a same named skill
check "each agent keeps its own alpha" \
    "grep -q 'The alpha skill' '$FIX/.zcode/skills/alpha/SKILL.md' && \
     grep -q 'A different alpha' '$FIX/.claude/skills/alpha/SKILL.md'"

check "the original directory was kept in the trash" \
    "[ -d \"\$(echo '$out' | python3 -c 'import json,sys; d=json.load(sys.stdin); print([a[\"backup\"] for a in d[\"agents\"] if a[\"id\"]==\"zcode\"][0])')/skills\" ]"

# scope "all": every agent sees every skill
"$BIN" link --scope all > /dev/null
check "with scope all both alpha variants are kept" \
    "[ -d '$FIX/.claude/skills/alpha' ] && [ -d '$FIX/.claude/skills/alpha__'* ]"
check "with scope all every agent sees the plugin skill" \
    "[ -f '$FIX/.cursor/skills/godot-2d/SKILL.md' ]"
"$BIN" link --scope own > /dev/null

# -------------------------------------------------------------- status --
echo "status"
state=$("$BIN" status --json | python3 -c '
import json,sys
d=json.load(sys.stdin)
print(" ".join(a["state"] for a in d["agents"]))')
check "every agent reports as linked" \
    "[ \"\$state\" = \"linked linked linked linked\" ]"

# -------------------------------------------------------------- unlink --
echo "unlink"
"$BIN" unlink --all > /dev/null
check "the original directory came back" "[ -d '$FIX/.zcode/skills' ] && [ ! -L '$FIX/.zcode/skills' ]"
check "its contents came back too" "[ -f '$FIX/.zcode/skills/alpha/SKILL.md' ]"
check "the claude directory came back" "[ -f '$FIX/.claude/skills/gamma/SKILL.md' ]"
check "the cursor directory came back" "[ -f '$FIX/.cursor/skills/crlf/SKILL.md' ]"

# ------------------------------------------------------------------ gc --
echo "gc"
out=$("$BIN" gc --json)
removed=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin)["removed"])')
check "gc removes the units nothing points at" "[ '$removed' = 7 ]"

# --------------------------------------------------------------- sync --
echo "sync"
mkdir -p "$FIX/.zcode/skills/delta"
printf -- '---\nname: delta\ndescription: Appeared later\n---\n' > "$FIX/.zcode/skills/delta/SKILL.md"
"$BIN" link > /dev/null
check "a newly appeared skill is adopted" "[ -f '$FIX/.zcode/skills/delta/SKILL.md' ]"

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
