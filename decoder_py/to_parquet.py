#!/usr/bin/env python3
"""Decode CME MDP3 captures for one channel into Parquet tables.

    python to_parquet.py <input.pcap.zst|.slim.zst> [more inputs] --out ../data.nosync/decoded/<name> [--channel 310]

Tables written to --out:
    packets.parquet       one row per UDP packet: capture time, connection, MsgSeqNum, SendingTime, gap
    book.parquet          template 46 MD entries (market by price: level, price, total qty, order count)
    orders.parquet        order events from template 47 and from the order group of template 46
    trades.parquet        template 48 MD entries (trade summary)
    trade_orders.parquet  template 48 order group (which orders were filled, and how much)
    other.parquet         every other template, fields stored as JSON
    stats.json            counts, sequence gaps and framing checks

Prices are int64 mantissas: divide by 1e9.
"""
import argparse
import json
import os
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mdp3 import Schema, iter_packet          # noqa: E402
from readers import channel_endpoints, ip_str, load_channels, read_any   # noqa: E402
from paths import config_file, schema_file                                # noqa: E402


i64, i32, u8, s = pa.int64(), pa.int32(), pa.uint8(), pa.string()
KEY = [("capture_ts_ns", i64), ("seq", i64), ("msg_i", pa.int16())]
SCHEMAS = {
    "packets": [("capture_ts_ns", i64), ("conn", s), ("seq", i64), ("send_ts_ns", i64),
                ("n_msgs", pa.int16()), ("leftover", pa.int16()), ("gap", i64)],
    "book": KEY + [("transact_ns", i64), ("match_event", u8), ("security_id", i32), ("rpt_seq", i64),
                   ("action", u8), ("side", s), ("level", u8), ("px", i64), ("qty", i32),
                   ("n_orders", i32), ("tradeable_qty", i32)],
    "orders": KEY + [("transact_ns", i64), ("src", u8), ("security_id", i32), ("order_id", pa.uint64()),
                     ("priority", pa.uint64()), ("px", i64), ("qty", i32), ("side", s), ("action", u8)],
    "trades": KEY + [("transact_ns", i64), ("security_id", i32), ("rpt_seq", i64), ("px", i64),
                     ("qty", i32), ("n_orders", i32), ("aggressor", u8), ("action", u8),
                     ("trade_id", pa.uint32())],
    "trade_orders": KEY + [("order_id", pa.uint64()), ("last_qty", i32)],
    "other": KEY + [("template", pa.uint16()), ("name", s), ("security_id", i32), ("json", s)],
}


class Sink:
    def __init__(self, out, flush_rows=250_000):
        self.out, self.flush_rows = Path(out), flush_rows
        self.out.mkdir(parents=True, exist_ok=True)
        self.buf = {t: defaultdict(list) for t in SCHEMAS}
        self.n = Counter()
        self.writers = {}

    def add(self, table, row):
        b = self.buf[table]
        for (name, _), v in zip(SCHEMAS[table], row):
            b[name].append(v)
        self.n[table] += 1
        if len(b["capture_ts_ns"]) >= self.flush_rows:
            self.flush(table)

    def flush(self, table):
        b = self.buf[table]
        if not b:
            return
        sch = pa.schema(SCHEMAS[table])
        t = pa.table({n: pa.array(b[n], type=ty) for n, ty in SCHEMAS[table]}, schema=sch)
        if table not in self.writers:
            self.writers[table] = pq.ParquetWriter(self.out / f"{table}.parquet", sch, compression="zstd")
        self.writers[table].write_table(t)
        self.buf[table] = defaultdict(list)

    def close(self):
        for t in SCHEMAS:
            self.flush(t)
            if t not in self.writers:            # write empty table so every file exists
                sch = pa.schema(SCHEMAS[t])
                pq.write_table(pa.table({n: pa.array([], type=ty) for n, ty in SCHEMAS[t]}, schema=sch),
                               self.out / f"{t}.parquet")
            else:
                self.writers[t].close()


def _jsonable(d):
    return {k: v for k, v in d.items() if not k.startswith("_")}


def run(inputs, out, channel, schema_path, config_path, limit=None):
    sch = Schema(schema_path)
    chmap = load_channels(config_path)
    want = channel_endpoints(chmap, channel)
    if not want:
        sys.exit(f"channel {channel} not in config.xml")
    sink = Sink(out)
    st = {"inputs": [str(p) for p in inputs], "channel": channel,
          "label": next(v["label"] for k, v in chmap.items() if v["channel"] == channel),
          "packets_by_conn": Counter(), "templates": Counter(), "unknown_templates": Counter(),
          "framing_leftover_packets": 0, "decode_length_mismatch": 0, "gaps": [], "dups_or_back": 0,
          "first_ts": None, "last_ts": None}
    last_seq = {}
    t0, npk = time.time(), 0
    for path in inputs:
        for ts, ip, port, p in read_any(path, want):
            conn = chmap[(ip, port)]["conn"]
            seq, send, msgs, left = iter_packet(p)
            npk += 1
            st["packets_by_conn"][conn] += 1
            st["first_ts"] = st["first_ts"] or ts
            st["last_ts"] = ts
            if left:
                st["framing_leftover_packets"] += 1
            prev = last_seq.get(conn)
            gap = 0 if prev is None else seq - prev - 1
            if prev is not None and gap > 0:
                st["gaps"].append({"conn": conn, "after": prev, "missing": gap, "ts": ts})
            if prev is not None and gap < 0:
                st["dups_or_back"] += 1
            if prev is None or seq > prev:
                last_seq[conn] = seq
            sink.add("packets", (ts, conn, seq, send, len(msgs), left, gap))
            for mi, m in enumerate(msgs):
                d = sch.decode(p, m)
                tid = m[0]
                if d is None:
                    st["unknown_templates"][tid] += 1
                    continue
                st["templates"][f"{tid} {d['_name']}"] += 1
                if d["_consumed"] != m[4] - m[3]:
                    st["decode_length_mismatch"] += 1
                k = (ts, seq, mi)
                if tid == 46:
                    ents = d["NoMDEntries"]
                    for e in ents:
                        sink.add("book", k + (d["TransactTime"], d["MatchEventIndicator"], e["SecurityID"],
                                              e["RptSeq"], e["MDUpdateAction"], e["MDEntryType"],
                                              e["MDPriceLevel"], e["MDEntryPx"], e["MDEntrySize"],
                                              e["NumberOfOrders"], e.get("TradeableSize")))
                    for o in d["NoOrderIDEntries"]:
                        ref = o["ReferenceID"]
                        e = ents[ref - 1] if ref and 0 < ref <= len(ents) else {}
                        sink.add("orders", k + (d["TransactTime"], 46, e.get("SecurityID"), o["OrderID"],
                                                o["MDOrderPriority"], e.get("MDEntryPx"), o["MDDisplayQty"],
                                                e.get("MDEntryType"), o["OrderUpdateAction"]))
                elif tid == 47:
                    for e in d["NoMDEntries"]:
                        sink.add("orders", k + (d["TransactTime"], 47, e["SecurityID"], e["OrderID"],
                                                e["MDOrderPriority"], e["MDEntryPx"], e["MDDisplayQty"],
                                                e["MDEntryType"], e["MDUpdateAction"]))
                elif tid == 48:
                    for e in d["NoMDEntries"]:
                        sink.add("trades", k + (d["TransactTime"], e["SecurityID"], e["RptSeq"], e["MDEntryPx"],
                                                e["MDEntrySize"], e["NumberOfOrders"], e["AggressorSide"],
                                                e["MDUpdateAction"], e["MDTradeEntryID"]))
                    for o in d["NoOrderIDEntries"]:
                        sink.add("trade_orders", k + (o["OrderID"], o["LastQty"]))
                else:
                    sid = d.get("SecurityID")
                    if sid is None and d.get("NoMDEntries"):
                        sid = d["NoMDEntries"][0].get("SecurityID")
                    sink.add("other", k + (tid, d["_name"], sid, json.dumps(_jsonable(d), default=str)))
            if limit and npk >= limit:
                break
    sink.close()
    st["rows"] = dict(sink.n)
    st["packets"] = npk
    st["seconds"] = round(time.time() - t0, 1)
    st["n_gaps"] = len(st["gaps"])
    st["gaps"] = st["gaps"][:200]
    for k in ("packets_by_conn", "templates", "unknown_templates"):
        st[k] = dict(st[k])
    (Path(out) / "stats.json").write_text(json.dumps(st, indent=1))
    return st


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--channel", type=int, default=310, help="CME channel id (310 = E-mini S&P 500 futures)")
    ap.add_argument("--schema", help="default: see decoder_py/paths.py")
    ap.add_argument("--config", help="default: see decoder_py/paths.py")
    ap.add_argument("--limit", type=int, help="stop after N packets (testing)")
    a = ap.parse_args()
    st = run(a.inputs, a.out, a.channel, a.schema or str(schema_file()), a.config or str(config_file()), a.limit)
    print(json.dumps({k: st[k] for k in ("packets", "seconds", "packets_by_conn", "templates", "rows",
                                         "n_gaps", "framing_leftover_packets", "decode_length_mismatch")},
                     indent=1))


if __name__ == "__main__":
    main()
