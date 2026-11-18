#!/usr/bin/env python3
"""Pull selected 10-minute CME captures from Box and keep only one channel (default 310 = ES).

Streams each file with `rclone cat` (nothing is cached on disk), keeps every connection of the
channel (incremental A/B, snapshot, instrument replay, MBO snapshot), and writes the same
lossless .slim.zst record format as the long-term capture archive:

    record = u64 ts_ns | u32 dst_ip | u16 dst_port | u16 len | <UDP payload>

Output: data.nosync/raw/<YYYYMMDD>/<channel>_<HHMMSS>.slim.zst plus a .json with sizes and
per-connection packet counts. Independent of the iCloud archive: it never touches its folders.

    python fetch_es.py 20250202T230000 20250327T133000 ...   [--channel 310] [--workers 3]
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import zstandard

HERE = Path(__file__).resolve().parent
IE = HERE.parent
ROOT = IE.parent
sys.path.insert(0, str(IE / "decoder_py"))
from readers import channel_endpoints, load_channels   # noqa: E402
from paths import config_file                           # noqa: E402

REMOTE = "box:db/glbx_pcaps"
REC = struct.Struct("<QIHH")


def fetch_one(stamp, channel, out_root):
    day = stamp[:8]
    name = f"dc3-glbx-a-{stamp}.pcap.zst"
    out_dir = out_root / day
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"ch{channel}_{stamp[9:]}.slim.zst"
    meta = out.with_suffix("").with_suffix(".json")
    if meta.exists():
        return json.loads(meta.read_text())
    chmap = load_channels(config_file())
    want = channel_endpoints(chmap, channel)
    t0 = time.time()
    proc = subprocess.Popen(["rclone", "cat", f"{REMOTE}/{day}", "--include", name],
                            stdout=subprocess.PIPE, bufsize=1 << 22)
    r = zstandard.ZstdDecompressor().stream_reader(proc.stdout, read_size=1 << 22, read_across_frames=True)
    import io
    s = io.BufferedReader(r, buffer_size=1 << 23)
    gh = s.read(24)
    if len(gh) < 24:
        raise RuntimeError(f"{name}: empty stream (file missing or rclone not configured)")
    magic = struct.unpack("<I", gh[:4])[0]
    e = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
    nano = magic in (0xA1B23C4D, 0x4D3CB2A1)
    rh = struct.Struct(e + "IIII")
    n_all = bytes_all = n_keep = bytes_keep = 0
    by_conn = Counter()
    tmp = out.with_suffix(".partial")
    with open(tmp, "wb") as fo:
        w = zstandard.ZstdCompressor(level=9).stream_writer(fo)
        while True:
            h = s.read(16)
            if len(h) < 16:
                break
            sec, frac, incl, _ = rh.unpack(h)
            d = s.read(incl)
            if len(d) < incl:
                break
            n_all += 1
            bytes_all += incl
            off = 14
            et = d[12:14]
            while et in (b"\x81\x00", b"\x88\xa8"):
                et = d[off + 2:off + 4]
                off += 4
            if et != b"\x08\x00" or len(d) < off + 28 or d[off + 9] != 17:
                continue
            dst = int.from_bytes(d[off + 16:off + 20], "big")
            u = off + (d[off] & 0x0F) * 4
            port = (d[u + 2] << 8) | d[u + 3]
            if (dst, port) not in want:
                continue
            ulen = (d[u + 4] << 8) | d[u + 5]
            p = d[u + 8:u + ulen]
            ts = sec * 1_000_000_000 + (frac if nano else frac * 1000)
            w.write(REC.pack(ts, dst, port, len(p)) + p)
            n_keep += 1
            bytes_keep += len(p)
            by_conn[chmap[(dst, port)]["conn"]] += 1
        w.close()
    proc.wait()
    if proc.returncode != 0:
        tmp.unlink(missing_ok=True)
        raise RuntimeError(f"{name}: rclone exit {proc.returncode}")
    tmp.rename(out)
    info = {"file": name, "out": str(out.relative_to(IE)), "seconds": round(time.time() - t0, 1),
            "packets_all": n_all, "pcap_bytes_all": bytes_all, "packets_kept": n_keep,
            "payload_bytes_kept": bytes_keep, "slim_zst_bytes": out.stat().st_size,
            "kept_share_of_bytes": round(bytes_keep / max(bytes_all, 1), 5), "packets_by_conn": dict(by_conn)}
    meta.write_text(json.dumps(info, indent=1))
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stamps", nargs="+", help="YYYYMMDDTHHMMSS of the 10-minute file (UTC)")
    ap.add_argument("--channel", type=int, default=310)
    ap.add_argument("--workers", type=int, default=3)
    a = ap.parse_args()
    out_root = IE / "data.nosync/raw"
    with ProcessPoolExecutor(a.workers) as ex:
        futs = {ex.submit(fetch_one, s, a.channel, out_root): s for s in a.stamps}
        for f in as_completed(futs):
            s = futs[f]
            try:
                i = f.result()
                print(f"[done] {s}: kept {i['packets_kept']:,} of {i['packets_all']:,} packets, "
                      f"{i['slim_zst_bytes'] / 1e6:.1f} MB, {i['seconds']} s, conns {i['packets_by_conn']}", flush=True)
            except Exception as ex_:
                print(f"[FAILED] {s}: {ex_}", flush=True)


if __name__ == "__main__":
    main()
