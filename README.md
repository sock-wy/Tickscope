# TickScope

A low-latency L3 (market-by-order) order book for CME E-mini S&P 500 futures (ES), built
from raw exchange packets, validated against the exchange's own book, and optimized step by
step with every step measured. Course project for IE 421 (High-Frequency Trading Technology), UIUC.

```
captured UDP packets ──► MDP 3.0 / SBE decode ──► L3 order book ──► top of book ──► strategy ──► order + snapshot out
 (CME Globex, channel 310)   templates 46, 47        every order       best bid/ask    stub         seqlock
```

## Status

| Step | State |
|---|---|
| 1. MDP 3.0 decoder | Python reference done; C++ decoder for the book templates done |
| 2. L3 book + validation vs CME MBP-10 | Done (Python and C++, identical results) |
| 3. Replay + 7-stage latency instrumentation | Done (back to back, recorded pace, stress) |
| 4. Code-level optimization (Mac) | 3 optimizations done, more planned |
| 5. System-level tuning (Linux) | Not started |
| 6. Live viewer (shared memory) | Snapshot publishing in place; viewer not started |
| 7. Report | Not started |

## Data

CME Globex packet captures (2025-02-01 to 2025-04-30, provided through the course) are **not**
in this repository. `tools/fetch_es.py` streams selected 10-minute files and keeps only
channel 310 (ES) as `.slim.zst` slices (`u64 ts_ns | u32 dst_ip | u16 dst_port | u16 len | payload`)
under `data.nosync/`, which git ignores. The CME schema files are also not included; see
`schema/README.md`.

Facts that shape the design:
- The captures hold the incremental feed only (no snapshots, no instrument definitions), so the
  book starts empty and orders resting before the capture start are unknown until they change.
- Tick sizes are therefore derived from the prices (gcd per security) instead of template 54.
- ES front month on 2025-03-27 is SecurityID 4916 (ESM5). Prices are `mantissa / 1e11`.

## Layout

| Path | Contents |
|---|---|
| `decoder_py/` | Python SBE decoder (`mdp3.py`), capture readers, Parquet export, packet inspector |
| `book_py/l3book.py` | Python reference L3 book and MBP-10 validation |
| `engine/` | C++20 engine: decoder, four book versions, replay with 7-stage timing, benchmarks, stress test (see `engine/README.md`) |
| `tools/fetch_es.py`, `fetch_es.command` | Pull capture slices from Box with rclone |
| `reports/` | Data structure report and L3 validation report |
| `engine/results/` | Raw benchmark and stress results (JSON) and summary tables |

## Correctness

At the end of every matching event, each price level CME publishes in its MBP-10 book
(template 46) is compared with the L3 book's aggregate at that price (quantity and order count).
2025-03-27 08:30–09:00 CT, ESM5: 12,945,176 levels compared, 88.053% exact. Every mismatch is
missing quantity from orders resting before the capture began (never extra quantity); in
08:44–08:53, when the visible range contains only orders seen in the capture, the match is 100.0%
at all ten levels. The Python reference and all four C++ book versions produce identical counts.

## Results so far (MacBook Pro, Apple Silicon, clang -O2)

Back-to-back replay of 2025-03-27 08:30–09:00 CT (1.6 M packets), median of 5 runs:

| Book version | What changed | Apply mean | Total mean | Total p99 | Total p99.9 | Packets/s |
|---|---|---|---|---|---|---|
| baseline | `std::map` levels, `std::unordered_map` orders, `std::list` queues | 59.5 ns | 101 ns | 417 ns | 1208 ns | 8.99 M |
| flat | price levels in a flat array indexed by tick | 43.0 | 84 | 292 | 875 | 10.73 M |
| pool | + order pool with intrusive queues (no per-order allocation) | 35.5 | 76 | 250 | 750 | 11.74 M |
| hash | + open-addressing OrderID index | **20.1** | **60** | **208** | **542** | **14.36 M** |

At recorded pace (x20) the book update is 3–4x slower than back to back (caches go cold
between packets): baseline 214 ns mean / 1708 ns p99, hash 73 ns / 458 ns.

Stress test (`engine/run_stress.sh`): the 2025-04-07 open, the heaviest day, replayed up to
x3000 its recorded pace, and three days merged (about 3x that load):

| Scenario | Speed | Offered avg | Baseline p99 | Hash p99 |
|---|---|---|---|---|
| 1 day | x300 | 514 k pkt/s | 10.7 µs | 5.3 µs |
| 1 day | x1000 | 1.71 M | 40.6 µs | 18.9 µs |
| 1 day | x3000 | 5.14 M | 410 µs | 127 µs |
| 3 days merged | x1000 | 3.94 M | 769 µs | 244 µs |
| 3 days merged | x3000 | 11.8 M | 120 ms (saturated) | 7.3 ms |

Back-to-back capacity drops from 8.7 M to 5.7 M packets/s for the baseline when three books are
live, but only from 12.8 M to 11.8 M for the hash version: its smaller working set stays in cache.
Timer: `mach_absolute_time` (41.67 ns ticks, so per-packet percentiles are multiples of 41.67 ns;
means are accurate). Full tables: `engine/results/summary.md`, `engine/results/stress_summary.md`.

## Running

```
# C++ benchmarks (needs data.nosync/raw/20250327/ch310_13{30,40,50}00.slim.zst)
bash engine/run_bench.sh      # validate all book versions, 5 timed runs each, paced runs
bash engine/run_stress.sh     # latency vs offered load

# Python reference
python book_py/l3book.py data.nosync/raw/20250327/ch310_133000.slim.zst --out out/ --security 4916
python decoder_py/inspect_mdp3.py data.nosync/raw/20250327/ch310_133000.slim.zst -n 5
```
Python needs `zstandard` (and `pyarrow` for the Parquet export).

## Next

- More code-level work: direct-indexed security table, fused decode/apply, hot/cold order layout,
  `-O3 -march=native`, LTO/PGO, cache warming between packets.
- Two threads (feed handler → book) over a lock-free SPSC queue; shared-memory snapshot for the live viewer.
- Linux system tuning (core isolation, pinning, frequency, huge pages) with `rdtsc` timing.
