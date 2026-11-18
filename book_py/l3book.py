#!/usr/bin/env python3
"""Reference L3 (market-by-order) order book for CME MDP3, with MBP-10 validation.

Reads raw ES packets (.slim.zst or .pcap.zst), decodes them with decoder_py/mdp3.py,
and maintains two books side by side:

  L3Book  every resting order: OrderID -> (security, side, price, qty, priority),
          aggregated per price level. Fed by the order entries of template 46
          (orders in the top 10 levels) and template 47 (orders outside them).
  MBP10   CME's own market-by-price top-10 book, rebuilt from the price-level
          entries of template 46. This is the reference answer.

At the end of every matching event (MatchEventIndicator bit 7) the two are compared
level by level for every security/side the event touched.

The capture has no snapshot, so orders resting before the first file are unknown.
The report therefore tracks the match rate over time: it should rise towards 100%
as the pre-existing orders are traded, cancelled or modified.

    python l3book.py <files...> --out <report dir> [--security 4916]

Prices are int64 mantissas (x 1e-9 x DisplayFactor; ES: price = px / 1e11).
"""
import argparse
import json
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "decoder_py"))
from mdp3 import Schema, iter_packet                  # noqa: E402
from readers import channel_endpoints, load_channels, read_any   # noqa: E402
from paths import config_file, schema_file                        # noqa: E402

ROOT = HERE.parents[1]

NEW, CHANGE, DELETE, DELETE_THRU, DELETE_FROM, OVERLAY = 0, 1, 2, 3, 4, 5
BID, ASK = "0", "1"                                   # MDEntryType for outright book sides
END_OF_EVENT = 0x80                                    # MatchEventIndicator bit 7
DEPTH = 10


class L3Book:
    """Every resting order, plus a per-level aggregate (qty, order count)."""

    def __init__(self):
        self.orders = {}                               # oid -> [sid, side, px, qty, prio]
        self.levels = defaultdict(dict)                # (sid, side) -> {px: [qty, n]}
        self.stats = Counter()

    def _add(self, sid, side, px, qty):
        lv = self.levels[(sid, side)].setdefault(px, [0, 0])
        lv[0] += qty
        lv[1] += 1

    def _remove(self, sid, side, px, qty):
        book = self.levels[(sid, side)]
        lv = book.get(px)
        if lv is None:
            self.stats["remove_missing_level"] += 1
            return
        lv[0] -= qty
        lv[1] -= 1
        if lv[1] <= 0:
            del book[px]

    def apply(self, oid, action, sid, side, px, qty, prio):
        o = self.orders.get(oid)
        if action == NEW:
            if o is not None:                          # re-add: treat as replace
                self._remove(*o[:3], o[3])
                self.stats["new_existing"] += 1
            self.orders[oid] = [sid, side, px, qty, prio]
            self._add(sid, side, px, qty)
            self.stats["new"] += 1
        elif action == CHANGE:
            if o is None:                              # pre-existing order we never saw: learn it
                self.orders[oid] = [sid, side, px, qty, prio]
                self._add(sid, side, px, qty)
                self.stats["change_unknown"] += 1
                return
            self._remove(o[0], o[1], o[2], o[3])
            o[1], o[2], o[3] = side, px, qty
            if prio is not None:
                o[4] = prio
            self._add(o[0], side, px, qty)
            self.stats["change"] += 1
        elif action == DELETE:
            if o is None:
                self.stats["delete_unknown"] += 1
                return
            self._remove(o[0], o[1], o[2], o[3])
            del self.orders[oid]
            self.stats["delete"] += 1

    def top(self, sid, side, n=DEPTH):
        book = self.levels.get((sid, side), {})
        prices = sorted(book, reverse=(side == BID))[:n]
        return [(p, book[p][0], book[p][1]) for p in prices]

    def queue(self, sid, side, px):
        """Orders at one price in time priority (lowest MDOrderPriority first)."""
        q = [(o[4], oid, o[3]) for oid, o in self.orders.items() if o[0] == sid and o[1] == side and o[2] == px]
        return sorted(q, key=lambda x: (x[0] is None, x[0]))


class MBP10:
    """CME's market-by-price book, as published in template 46 (reference answer)."""

    def __init__(self):
        self.book = defaultdict(lambda: [None] * DEPTH)   # (sid, side) -> [ (px, qty, n) | None ]

    def apply(self, sid, side, level, action, px, qty, n):
        b = self.book[(sid, side)]
        i = level - 1
        if not 0 <= i < DEPTH:
            return
        if action == NEW:
            b.insert(i, (px, qty, n))
            b.pop()
        elif action in (CHANGE, OVERLAY):
            b[i] = (px, qty, n)
        elif action == DELETE:
            b.pop(i)
            b.append(None)
        elif action == DELETE_THRU:
            b[:] = [None] * DEPTH
        elif action == DELETE_FROM:
            for j in range(i, DEPTH):
                b[j] = None


def compare(l3, mbp, sid, side):
    """Compare every MBP level we know with the L3 aggregate at the same price."""
    book = l3.levels.get((sid, side), {})
    res = Counter()
    for k, lv in enumerate(mbp.book[(sid, side)], 1):
        if lv is None:
            continue
        px, qty, n = lv
        res["levels"] += 1
        res[f"L{k}"] += 1
        got = book.get(px)
        if got is not None and got[0] == qty and got[1] == n:
            res["match"] += 1
            res[f"L{k}_match"] += 1
        else:
            res["qty_short"] += max(0, qty - (got[0] if got else 0))
            res["qty_extra"] += max(0, (got[0] if got else 0) - qty)
    return res


def run(files, out, only_sid=None, sample_every=60):
    sch = Schema(schema_file())
    chmap = load_channels(config_file())
    want = channel_endpoints(chmap, 310)
    l3, mbp = L3Book(), MBP10()
    touched = set()
    total = Counter()
    per_min = defaultdict(Counter)                     # minute -> counts
    examples = []
    events = 0
    t0 = time.time()
    last_seq = None
    gaps = 0
    first_ts = last_ts = None
    for path in files:
        for ts, ip, port, p in read_any(path, want):
            seq, _send, msgs, _left = iter_packet(p)
            if last_seq is not None and seq != last_seq + 1:
                gaps += 1
            last_seq = seq
            first_ts = first_ts or ts
            last_ts = ts
            for m in msgs:
                tid = m[0]
                if tid not in (46, 47):
                    continue
                d = sch.decode(p, m)
                if tid == 46:
                    ents = d["NoMDEntries"]
                    for o in d["NoOrderIDEntries"]:
                        ref = o["ReferenceID"]
                        if not ref or ref > len(ents):
                            total["order_no_ref"] += 1
                            continue
                        e = ents[ref - 1]
                        if e["MDEntryType"] not in (BID, ASK):
                            continue
                        l3.apply(o["OrderID"], o["OrderUpdateAction"], e["SecurityID"], e["MDEntryType"],
                                 e["MDEntryPx"], o["MDDisplayQty"] or 0, o["MDOrderPriority"])
                    for e in ents:
                        if e["MDEntryType"] not in (BID, ASK):
                            continue                   # implied prices and book resets are not orders
                        mbp.apply(e["SecurityID"], e["MDEntryType"], e["MDPriceLevel"], e["MDUpdateAction"],
                                  e["MDEntryPx"], e["MDEntrySize"], e["NumberOfOrders"])
                        touched.add((e["SecurityID"], e["MDEntryType"]))
                else:
                    for e in d["NoMDEntries"]:
                        if e["MDEntryType"] not in (BID, ASK):
                            continue
                        l3.apply(e["OrderID"], e["MDUpdateAction"], e["SecurityID"], e["MDEntryType"],
                                 e["MDEntryPx"], e["MDDisplayQty"] or 0, e["MDOrderPriority"])
                if d["MatchEventIndicator"] & END_OF_EVENT and touched:
                    events += 1
                    minute = ts // 60_000_000_000
                    for sid, side in touched:
                        if only_sid is not None and sid != only_sid:
                            continue
                        r = compare(l3, mbp, sid, side)
                        total.update(r)
                        per_min[minute].update(r)
                        if r["levels"] != r["match"] and len(examples) < 20 and events % 5000 == 0:
                            examples.append({"ts": ts, "sid": sid, "side": side,
                                             "mbp": [lv for lv in mbp.book[(sid, side)] if lv],
                                             "l3": l3.top(sid, side)})
                    touched.clear()
    secs = time.time() - t0
    rows = []
    for minute in sorted(per_min):
        c = per_min[minute]
        rows.append({"minute_utc": time.strftime("%H:%M", time.gmtime(minute * 60)),
                     "levels": c["levels"], "match_pct": round(100 * c["match"] / max(c["levels"], 1), 3),
                     "top1_match_pct": round(100 * c["L1_match"] / max(c["L1"], 1), 3),
                     "qty_short": c["qty_short"], "qty_extra": c["qty_extra"]})
    by_level = {f"L{k}": round(100 * total[f"L{k}_match"] / max(total[f"L{k}"], 1), 3) for k in range(1, DEPTH + 1)}
    report = {
        "files": [str(f) for f in files], "security": only_sid, "seconds": round(secs, 1),
        "span_utc": [time.strftime("%H:%M:%S", time.gmtime(first_ts / 1e9)),
                     time.strftime("%H:%M:%S", time.gmtime(last_ts / 1e9))],
        "sequence_gaps": gaps, "events_compared": events,
        "levels_compared": total["levels"], "levels_matched": total["match"],
        "match_pct": round(100 * total["match"] / max(total["levels"], 1), 3),
        "match_pct_by_level": by_level,
        "order_events": dict(l3.stats), "orders_resting_at_end": len(l3.orders),
        "per_minute": rows, "examples": examples,
    }
    Path(out).mkdir(parents=True, exist_ok=True)
    Path(out, "validation.json").write_text(json.dumps(report, indent=1, default=str))
    return report, l3, mbp


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--security", type=int, help="validate only this SecurityID (e.g. 4916 = ESM5)")
    a = ap.parse_args()
    rep, _, _ = run(a.files, a.out, a.security)
    print(json.dumps({k: rep[k] for k in ("span_utc", "seconds", "sequence_gaps", "events_compared",
                                           "levels_compared", "levels_matched", "match_pct", "match_pct_by_level",
                                           "order_events", "orders_resting_at_end")}, indent=1))
    for r in rep["per_minute"]:
        print(r)


if __name__ == "__main__":
    main()
