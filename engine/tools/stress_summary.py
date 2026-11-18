#!/usr/bin/env python3
"""Latency vs offered load, from stress-test result files (run_stress.sh).

One table per (scenario, book): each row is one replay speed. "Offered" is the packet rate
the engine is asked to absorb at that speed (average and busiest millisecond); "back to back"
is the engine's own maximum. Total latency runs from the moment a packet is due to the moment
its order/snapshot is out, so it includes waiting behind earlier packets.

    python stress_summary.py results/stress_*.json [--out results/stress_summary.md]
"""
import argparse
import json
from collections import defaultdict
from pathlib import Path


def fmt_rate(x):
    return f"{x / 1e6:.2f} M" if x >= 1e6 else f"{x / 1e3:.0f} k"


def fmt_ns(x):
    if x >= 1e6:
        return f"{x / 1e6:.1f} ms"
    return f"{x / 1e3:.1f} µs" if x >= 1e4 else f"{x:.0f} ns"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()
    groups = defaultdict(list)
    for f in a.files:
        r = json.loads(Path(f).read_text())
        scen = "1 day (2025-04-07 open)" if r["load"]["streams"] == 1 else f"{r['load']['streams']} days merged"
        groups[(scen, r["engine"])].append(r)
    out = []
    for (scen, eng), runs in sorted(groups.items()):
        runs.sort(key=lambda r: (r["speed"] == 0, r["speed"]))
        ld = runs[0]["load"]
        out.append(f"### {scen}, {eng} — recorded {fmt_rate(ld['recorded_pps'])} pkt/s on average, "
                   f"busiest ms {ld['recorded_peak_per_ms']} packets\n")
        out.append("| speed | offered avg | offered peak | total p50 | p99 | p99.9 | max | queue p99 |")
        out.append("|---|---|---|---|---|---|---|---|")
        for r in runs:
            L, t, q = r["load"], r["latency_ns_per_packet"]["total"], r["latency_ns_per_packet"]["1_queue"]
            sp = "back to back" if r["speed"] == 0 else f"x{r['speed']:g}"
            peak = "—" if r["speed"] == 0 else fmt_rate(L["offered_peak_pps"])
            out.append(f"| {sp} | {fmt_rate(L['offered_pps'])} | {peak} | {fmt_ns(t['p50'])} | {fmt_ns(t['p99'])} | "
                       f"{fmt_ns(t['p99.9'])} | {fmt_ns(t['max'])} | {fmt_ns(q['p99'])} |")
        out.append("")
    text = "\n".join(out)
    print(text)
    if a.out:
        Path(a.out).write_text(text + "\n")


if __name__ == "__main__":
    main()
