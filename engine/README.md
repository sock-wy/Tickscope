# TickScope engine (C++)

C++20, header-only core, no external libraries.

| File | Role |
|---|---|
| `include/tickscope/mdp3.hpp` | MDP 3.0 / SBE decoding (schema v13): `frame_packet` (stage 2) and `decode_frames` (stage 3) for templates 46 and 47 |
| `include/tickscope/book_baseline.hpp` | **Baseline L3 book**, the standard design: `std::unordered_map` orders, `std::map` price levels, `std::list` queues, default allocator |
| `include/tickscope/book_flat.hpp` | **Optimization 1** (`flat`): price levels in a flat array indexed by tick; everything else as in the baseline |
| `include/tickscope/book_pool.hpp` | **Optimization 2** (`pool`): + orders in one contiguous pool with intrusive level queues (no per-order heap allocation). **Optimization 3** (`hash`): + open-addressing OrderID index instead of `std::unordered_map` |
| `include/tickscope/mbp10.hpp` | CME's MBP-10 rebuilt from template 46, for validation only |
| `include/tickscope/clock.hpp` | Timestamps: `mach_absolute_time` on macOS, `rdtsc` on x86-64; resolution, per-read overhead and monotonicity checked at start (replay refuses to run on a bad clock) |
| `src/replay.cpp` | Loads captures into memory, replays them (back to back or at recorded pace), times 7 stages per packet, optional validation |
| `tools/prep_slim.py` | Decompresses `.slim.zst` slices to `.slim` |
| `tools/summarize.py` | Median-of-runs comparison table → `results/summary.md` |
| `tools/stress_summary.py` | Latency-vs-load tables from stress runs → `results/stress_summary.md` |
| `run_stress.sh` | Stress test: 2025-04-07 open alone and three days merged, replayed at x10 … x3000 and back to back |
| `run_bench.sh` | Prepare, build, validate every book, time 5 interleaved runs each + one paced run (x20) |

## Run
```
bash engine/run_bench.sh     # from the repository root; data in data.nosync/ (see top-level README)
```
Takes about 8 minutes (each paced run is 90 s).

## Stress test
`run_stress.sh` (about 6 minutes) answers "how much load before latency blows up":
- **One day**: 2025-04-07 08:30–08:40 CT, the heaviest day in the data (1,713 packets/s on average).
- **Three days merged**: the 03-27, 04-07 and 04-10 opens interleaved by time since their own open
  (`--merge`; IDs remapped per day so the books stay separate), about 3x the heaviest real day.
- Each is replayed at x10, x30, x100, x300, x1000, x3000 its recorded pace and back to back.
  Total latency is measured from the moment a packet is due, so it includes queueing.
  The knee is where p99 jumps from nanoseconds to micro/milliseconds.

## The 7 stages (per packet, timestamps T0..T7)
| # | Stage | From → to |
|---|---|---|
| 1 | queue | packet due (paced) or loop start → processing starts |
| 2 | parse | packet header, sequence check, message framing |
| 3 | decode | message bodies → order / level events |
| 4 | apply | order events into the L3 book |
| 5 | top of book | best bid / ask of every security the packet touched |
| 6 | strategy | decision on the front month (stub: 4x size imbalance) |
| 7 | out | encode the order, publish the top-of-book snapshot (seqlock) |

## Measurement notes
- Every timestamp read costs the clock overhead reported in each result file; it is included in every stage.
- Apple Silicon's timer (`mach_absolute_time`) ticks every 41.67 ns, so per-packet percentiles on the Mac are multiples of
  41.67 ns. Means over 1.6 M packets are still accurate (rounding errors average out).
  x86-64 Linux (`rdtsc`, < 1 ns) is used for fine per-packet distributions (system-tuning step).
- Back-to-back replay keeps caches warm; recorded-pace replay (`--speed`) is closer to live
  conditions and also measures queueing during bursts.

## Correctness
Every book version must reproduce the Python reference (`book_py/l3book.py`) on 2025-03-27
08:30–09:00 CT: 1,479,613 events, 12,945,176 levels compared, 11,398,635 matched (88.053%).
All four versions (baseline, flat, pool, hash) do.

## Notes from development
- The first Mac run read the ARM generic timer (`cntvct_el0`) directly and got non-monotonic values,
  which made every timing meaningless. macOS now uses `mach_absolute_time`, and every run checks the
  clock (1 M back-to-back reads, no backward steps allowed) before timing.
- The open-addressing table starts at 4,096 slots and doubles at 50% load. Starting at 65,536 slots
  made p99 apply latency worse (about 740 ns vs 270 ns on a Linux test machine): the larger table no longer
  fits in cache, so lookups miss even though the load factor is lower.
