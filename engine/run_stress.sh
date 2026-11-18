#!/bin/bash
# Stress test: latency vs offered load on real CME data (ES, channel 310).
#   A. one day: 2025-04-07 (tariff sell-off, heaviest day) 08:30-08:40 CT, replayed at
#      x10 ... x3000 its recorded pace and back to back, for the baseline and the fastest book
#   B. three days merged: 2025-03-27, 04-07 and 04-10 opens interleaved (about 3x the
#      heaviest real day), same speeds
# Total time about 6 minutes. Results: results/stress_*.json and results/stress_summary.md
# Run from Terminal:  bash "<this folder>/run_stress.sh"
set -e
cd "$(dirname "$0")"
PY="../../.venv.nosync/bin/python"
[ -x "$PY" ] || PY=python3
SLIM=../data.nosync/slim
for d in 20250327 20250407 20250410; do
  "$PY" tools/prep_slim.py ../data.nosync/raw/$d/ch310_133000.slim.zst --out $SLIM/$d
done

mkdir -p build results
rm -f results/stress_*.json
clang++ -std=c++20 -O2 -DNDEBUG -Iinclude src/replay.cpp -o build/replay
ONE="$SLIM/20250407/ch310_133000.slim"
THREE="$SLIM/20250327/ch310_133000.slim $SLIM/20250407/ch310_133000.slim $SLIM/20250410/ch310_133000.slim"
SPEEDS="10 30 100 300 1000 3000 0"

for b in baseline hash; do
  echo ">> A. one day, $b"
  for s in $SPEEDS; do
    build/replay --book $b --speed $s --max-seconds 600 --out results/stress_1day_${b}_x$s.json $ONE > /dev/null
    printf "   x%s" $s
  done; echo
  echo ">> B. three days merged, $b"
  for s in $SPEEDS; do
    build/replay --book $b --merge --speed $s --max-seconds 600 --out results/stress_3day_${b}_x$s.json $THREE > /dev/null
    printf "   x%s" $s
  done; echo
done
echo
"$PY" tools/stress_summary.py results/stress_*.json --out results/stress_summary.md
echo ">> Saved results/stress_summary.md. Tell Claude when done."
