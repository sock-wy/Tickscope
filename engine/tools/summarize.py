#!/usr/bin/env python3
"""Summarize replay result files into one comparison table (markdown).

For each (book, speed) group it takes the median across runs of every statistic, so a
single noisy run does not decide the comparison.

    python summarize.py results/*.json [--out results/summary.md]
"""
import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path

STAGES = ["1_queue", "2_parse", "3_decode", "4_apply", "5_top_of_book", "6_strategy", "7_out", "total"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()
    groups = defaultdict(list)
    for f in a.files:
        r = json.loads(Path(f).read_text())
        if r.get("mode") != "timing":
            continue
        groups[(r["engine"], r.get("speed", 0))].append(r)
    lines = []
    clock = next(iter(groups.values()))[0]["clock"] if groups else {}
    lines.append(f"Clock: {clock.get('source')}, resolution {clock.get('resolution_ns', 0):.2f} ns, "
                 f"overhead {clock.get('overhead_ns', 0):.1f} ns per read. Median of runs shown.\n")
    for (engine, speed), runs in sorted(groups.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        pace = "back to back" if not speed else f"recorded pace x{speed:g}"
        pps = statistics.median(r["packets_per_second"] for r in runs)
        lines.append(f"### {engine} ({pace}, {len(runs)} runs, {pps / 1e6:.2f} M packets/s)\n")
        lines.append("| stage | mean ns | p50 | p99 | p99.9 |")
        lines.append("|---|---|---|---|---|")
        for s in STAGES:
            med = lambda k: statistics.median(r["latency_ns_per_packet"][s][k] for r in runs)
            lines.append(f"| {s} | {med('mean'):.1f} | {med('p50'):.0f} | {med('p99'):.0f} | {med('p99.9'):.0f} |")
        lines.append("")
    text = "\n".join(lines)
    print(text)
    if a.out:
        Path(a.out).write_text(text + "\n")


if __name__ == "__main__":
    main()
