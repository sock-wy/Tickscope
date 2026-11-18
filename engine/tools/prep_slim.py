#!/usr/bin/env python3
"""Decompress .slim.zst capture slices into plain .slim files for the C++ replay.

The replay loads whole files into memory before timing starts, so it reads the plain
format directly and needs no zstd library.

    python prep_slim.py ../data.nosync/raw/20250327/ch310_13*.slim.zst --out ../data.nosync/slim/20250327
"""
import argparse
from pathlib import Path

import zstandard


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for f in a.files:
        src = Path(f)
        dst = out / src.name.removesuffix(".zst")
        if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
            print(f"have {dst.name}")
            continue
        with open(src, "rb") as fi, open(dst, "wb") as fo:
            zstandard.ZstdDecompressor().copy_stream(fi, fo)
        print(f"wrote {dst.name} ({dst.stat().st_size / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
