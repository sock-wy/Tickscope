#!/bin/bash
# Build TickScope and compare order-book versions on 2025-03-27 08:30-09:00 CT (ES, channel 310).
#   1. decompress the three 10-minute slices (once)
#   2. compile with clang++ -O2
#   3. validate every book version (must equal the Python reference: 11,398,635 levels, 88.053%)
#   4. timing: 5 runs per version, interleaved, back to back
#   5. timing at recorded pace x20 (90 s per version): measures queueing during bursts
#   6. summary table -> results/summary.md
# Run from Terminal:  bash "<this folder>/run_bench.sh"
set -e
cd "$(dirname "$0")"
PY="../../.venv.nosync/bin/python"
[ -x "$PY" ] || PY=python3
BOOKS="baseline flat pool hash"
RAW=../data.nosync/raw/20250327
SLIM=../data.nosync/slim/20250327
"$PY" tools/prep_slim.py $RAW/ch310_133000.slim.zst $RAW/ch310_134000.slim.zst $RAW/ch310_135000.slim.zst --out $SLIM

mkdir -p build results
rm -f results/*.json
clang++ -std=c++20 -O2 -DNDEBUG -Iinclude src/replay.cpp -o build/replay
FILES="$SLIM/ch310_133000.slim $SLIM/ch310_134000.slim $SLIM/ch310_135000.slim"

echo; echo ">> validation (ESM5 = SecurityID 4916)"
for b in $BOOKS; do
  printf "%-9s " $b
  build/replay --book $b --validate 4916 --out results/validate_$b.json $FILES | grep -o '"levels_matched": [0-9]*, "match_pct": [0-9.]*'
done

echo; echo ">> timing, back to back (5 runs each)"
for i in 1 2 3 4 5; do
  for b in $BOOKS; do
    build/replay --book $b --out results/timing_${b}_max_run$i.json $FILES > /dev/null
  done
  echo "   run $i done"
done

echo; echo ">> timing at recorded pace x20 (about 90 s per version, 6 minutes total)"
for b in $BOOKS; do
  build/replay --book $b --speed 20 --out results/timing_${b}_x20.json $FILES > /dev/null
  echo "   $b done"
done

echo
"$PY" tools/summarize.py results/timing_*.json --out results/summary.md
echo ">> Saved results/summary.md. Tell Claude when done."
