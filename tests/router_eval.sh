#!/bin/bash
# router_eval.sh - score the skill router against a labeled query set.
#
# Tuning/sanity helper, not part of `make test`: expectations name skills that
# only exist on a populated machine.
#
# Usage:
#   bash tests/router_eval.sh [--bin PATH] [--tsv PATH] [--limit N] [--verbose]
#
#   --bin PATH    router binary           (default: $SKILLCTL_BIN, else $ROOT/build/skillctl)
#   --tsv PATH    query set, TAB separated (default: $ROOT/tests/router_queries.tsv)
#   --limit N     results requested per query (default: 10)
#   --verbose     print the top hits and any stderr for every query
#
# Query-set format: one "query<TAB>expected" per line, '#' starts a comment.
# expected is either
#   NAME          hit any catalog entry with that name (duplicate names allowed)
#   NAME@HASH8    hit only the entry whose id starts with the 8-char hash prefix
#
# Metrics (one relevant entry per query, so recall is 0/1):
#   Hit@1, MRR@10, Recall@3, Recall@10, nDCG@10 (single-gain, IDCG=1).
# The final stdout line is always:
#   METRICS queries=N hit1=x.xxx mrr10=x.xxx recall3=x.xxx recall10=x.xxx ndcg10=x.xxx
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${SKILLCTL_BIN:-$ROOT/build/skillctl}"
TSV="$ROOT/tests/router_queries.tsv"
LIMIT=10
VERBOSE=0
POSITIONAL=""

usage() {
    sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --bin)
            [ "$#" -ge 2 ] || { echo "router_eval: --bin needs a path" >&2; exit 64; }
            BIN="$2"; shift 2 ;;
        --bin=*)   BIN="${1#--bin=}";   shift ;;
        --tsv)
            [ "$#" -ge 2 ] || { echo "router_eval: --tsv needs a path" >&2; exit 64; }
            TSV="$2"; shift 2 ;;
        --tsv=*)   TSV="${1#--tsv=}";   shift ;;
        --limit)
            [ "$#" -ge 2 ] || { echo "router_eval: --limit needs a number" >&2; exit 64; }
            LIMIT="$2"; shift 2 ;;
        --limit=*) LIMIT="${1#--limit=}"; shift ;;
        --verbose|-v) VERBOSE=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *)
            if [ -n "$POSITIONAL" ]; then
                echo "router_eval: unexpected argument: $1" >&2; exit 64
            fi
            POSITIONAL="$1"; shift ;;      # backwards compatible: bare TSV path
    esac
done

[ -n "$POSITIONAL" ] && TSV="$POSITIONAL"

case "$LIMIT" in
    *[!0-9]*|"") echo "router_eval: --limit must be a positive integer, got '$LIMIT'" >&2; exit 64 ;;
    0)           echo "router_eval: --limit must be >= 1" >&2; exit 64 ;;
esac

[ -x "$BIN" ] || { echo "build the binary first, or pass --bin: $BIN" >&2; exit 1; }
[ -r "$TSV" ] || { echo "missing query set: $TSV" >&2; exit 2; }

python3 - "$BIN" "$TSV" "$LIMIT" "$VERBOSE" <<'PY'
import json
import math
import os
import subprocess
import sys

bin_path, tsv, limit_s, verbose_s = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
limit = int(limit_s)
verbose = verbose_s == "1"

rows = []
malformed = []
with open(tsv, encoding="utf-8", errors="replace") as fh:
    for lineno, raw in enumerate(fh, 1):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if "\t" not in line:
            malformed.append((lineno, "no TAB separator", line))
            continue
        query, expected = line.split("\t", 1)
        query, expected = query.strip(), expected.strip()
        if not query or not expected:
            malformed.append((lineno, "empty query or expected", line))
            continue
        if expected.count("@") > 1:
            malformed.append((lineno, "expected has more than one '@'", line))
            continue
        rows.append((lineno, query, expected))

if malformed:
    print("router_eval: malformed query set", file=sys.stderr)
    for lineno, why, line in malformed:
        print(f"  line {lineno}: {why}: {line!r}", file=sys.stderr)
    sys.exit(2)

if not rows:
    print(f"router_eval: no queries in {tsv}", file=sys.stderr)
    sys.exit(3)

if verbose:
    print(f"# bin   {bin_path}")
    print(f"# tsv   {tsv}")
    print(f"# limit {limit}")
    if limit < 10:
        print(f"# note  limit<10: recall10/ndcg10 and MRR10 can only see the top {limit}")

def parse_expected(expected):
    if "@" in expected:
        name, hash8 = expected.rsplit("@", 1)
        return name, hash8.lower()
    return expected, None

def rank_of(hits, expected):
    name, hash8 = parse_expected(expected)
    for i, hit in enumerate(hits, 1):
        if hit.get("name") != name:
            continue
        if hash8 is None:
            return i
        if str(hit.get("id", "")).lower().startswith(hash8):
            return i
    return 0

query_errors = 0
results = []
for lineno, query, expected in rows:
    proc = subprocess.run(
        [bin_path, "find", "--json", "--limit", str(limit)]
        + (["--mode", os.environ["ROUTER_MODE"]] if os.environ.get("ROUTER_MODE") else [])
        + [query],
        capture_output=True, text=True,
    )
    hits = []
    if proc.returncode != 0:
        query_errors += 1
        if verbose:
            print(f"# exit {proc.returncode} for {query!r}: {proc.stderr.strip()}")
    else:
        raw = proc.stdout.strip()
        if raw:
            try:
                hits = json.loads(raw)
            except json.JSONDecodeError as exc:
                query_errors += 1
                hits = []
                if verbose:
                    print(f"# bad JSON for {query!r}: {exc}")
        if not isinstance(hits, list):
            hits = []
    rank = rank_of(hits, expected)
    results.append((rank, query, expected))
    if verbose:
        shown = ", ".join(
            f"{h.get('name')}@{str(h.get('id', ''))[:8]}({h.get('score')})"
            for h in hits[:3]
            if isinstance(h, dict)
        )
        print(f"# {query!r} -> {expected} | rank={rank} | top: {shown or '(none)'}")

n = len(results)
hit1 = mrr10 = recall3 = recall10 = ndcg10 = 0.0
for rank, _query, _expected in results:
    if rank <= 0:
        continue
    if rank == 1:
        hit1 += 1.0
    if rank <= 3:
        recall3 += 1.0
    if rank <= 10:
        recall10 += 1.0
        mrr10 += 1.0 / rank
        ndcg10 += 1.0 / math.log2(1 + rank)

for rank, query, expected in results:
    mark = "ok" if rank == 1 else ("late" if rank else "MISS")
    print(f"  {mark} rank={rank}  {query}  ->  {expected}")

if query_errors:
    print()
    print(f"# {query_errors} query command(s) failed or returned non-JSON; counted as no hit",
          file=sys.stderr)

print()
print(
    f"METRICS queries={n}"
    f" hit1={hit1 / n:.3f}"
    f" mrr10={mrr10 / n:.3f}"
    f" recall3={recall3 / n:.3f}"
    f" recall10={recall10 / n:.3f}"
    f" ndcg10={ndcg10 / n:.3f}"
)
PY
