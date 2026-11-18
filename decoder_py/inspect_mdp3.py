#!/usr/bin/env python3
"""Print CME MDP3 packets in readable form.

    python inspect_mdp3.py <file> [--channel 310] [-n 5] [--skip 0] [--template 47]

Prices are shown as decimals: mantissa * 1e-9 * DisplayFactor (0.01 for ES).
Enum values are shown with their names.
"""
import argparse
import os
import sys
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mdp3 import Schema, iter_packet          # noqa: E402
from readers import channel_endpoints, load_channels, read_any   # noqa: E402
from paths import config_file, schema_file  # noqa: E402

PRICE_FIELDS = {"MDEntryPx", "HighLimitPrice", "LowLimitPrice", "MaxPriceVariation",
                "MinPriceIncrement", "TradingReferencePrice", "DisplayFactor"}
ENUMS = {"MDUpdateAction": "MDUpdateAction", "OrderUpdateAction": "OrderUpdateAction",
         "AggressorSide": "AggressorSide", "MDEntryType": "MDEntryTypeBook"}


def ts(ns):
    if ns is None:
        return "-"
    return datetime.fromtimestamp(ns / 1e9, timezone.utc).strftime("%H:%M:%S.") + f"{ns % 1_000_000_000:09d}"


DISPLAY_FACTOR = 0.01   # ES: real price = mantissa * 1e-9 * DisplayFactor (0.01 for ES)


def fmt(sch, k, v):
    if k in PRICE_FIELDS and isinstance(v, int):
        return f"{v * 1e-9 * DISPLAY_FACTOR:g}"
    if k in ENUMS:
        n = sch.enum_name(ENUMS[k], v)
        return f"{v}({n})" if n != v else str(v)
    if k == "TransactTime":
        return ts(v)
    return str(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--channel", type=int, default=310)
    ap.add_argument("-n", type=int, default=5, help="packets to print")
    ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("--template", type=int, help="only packets containing this template")
    ap.add_argument("--display-factor", type=float, default=0.01, help="instrument DisplayFactor (ES = 0.01)")
    a = ap.parse_args()
    global DISPLAY_FACTOR
    DISPLAY_FACTOR = a.display_factor
    sch = Schema(schema_file())
    chmap = load_channels(config_file())
    shown = seen = 0
    for cts, ip, port, p in read_any(a.file, channel_endpoints(chmap, a.channel)):
        seq, send, msgs, left = iter_packet(p)
        if a.template and not any(m[0] == a.template for m in msgs):
            continue
        seen += 1
        if seen <= a.skip:
            continue
        print(f"=== packet  conn={chmap[(ip, port)]['conn']}  seq={seq}  captured={ts(cts)}  "
              f"sent={ts(send)}  bytes={len(p)}  msgs={len(msgs)}  leftover={left}")
        for m in msgs:
            d = sch.decode(p, m)
            if d is None:
                print(f"  [template {m[0]} unknown]")
                continue
            head = {k: v for k, v in d.items() if not k.startswith("_") and not isinstance(v, list)}
            print(f"  -- {d['_name']} (id {m[0]}, {m[4] - m[3]} B body)  "
                  + "  ".join(f"{k}={fmt(sch, k, v)}" for k, v in head.items()))
            for g, rows in d.items():
                if isinstance(rows, list):
                    print(f"     {g}: {len(rows)} entries")
                    for r in rows:
                        print("       " + "  ".join(f"{k}={fmt(sch, k, v)}" for k, v in r.items()))
        shown += 1
        if shown >= a.n:
            break


if __name__ == "__main__":
    main()
