Clock: mach_absolute_time, resolution 41.67 ns, overhead 5.4 ns per read. Median of runs shown.

### baseline (back to back, 5 runs, 8.99 M packets/s)

| stage | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| 1_queue | 4.6 | 0 | 42 | 42 |
| 2_parse | 7.2 | 0 | 42 | 42 |
| 3_decode | 10.7 | 0 | 42 | 125 |
| 4_apply | 59.5 | 42 | 333 | 958 |
| 5_top_of_book | 8.9 | 0 | 42 | 83 |
| 6_strategy | 5.4 | 0 | 42 | 42 |
| 7_out | 5.0 | 0 | 42 | 42 |
| total | 101.4 | 83 | 417 | 1208 |

### flat (back to back, 5 runs, 10.73 M packets/s)

| stage | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| 1_queue | 4.6 | 0 | 42 | 42 |
| 2_parse | 5.9 | 0 | 42 | 42 |
| 3_decode | 11.0 | 0 | 42 | 125 |
| 4_apply | 43.0 | 42 | 208 | 625 |
| 5_top_of_book | 9.1 | 0 | 42 | 83 |
| 6_strategy | 4.6 | 0 | 42 | 42 |
| 7_out | 5.3 | 0 | 42 | 42 |
| total | 83.6 | 83 | 292 | 875 |

### hash (back to back, 5 runs, 14.36 M packets/s)

| stage | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| 1_queue | 4.6 | 0 | 42 | 42 |
| 2_parse | 5.7 | 0 | 42 | 42 |
| 3_decode | 10.9 | 0 | 42 | 125 |
| 4_apply | 20.1 | 0 | 125 | 333 |
| 5_top_of_book | 9.3 | 0 | 42 | 83 |
| 6_strategy | 4.6 | 0 | 42 | 42 |
| 7_out | 4.6 | 0 | 42 | 42 |
| total | 60.2 | 42 | 208 | 542 |

### pool (back to back, 5 runs, 11.74 M packets/s)

| stage | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| 1_queue | 4.6 | 0 | 42 | 42 |
| 2_parse | 5.8 | 0 | 42 | 42 |
| 3_decode | 10.8 | 0 | 42 | 125 |
| 4_apply | 35.5 | 42 | 167 | 542 |
| 5_top_of_book | 9.7 | 0 | 42 | 83 |
| 6_strategy | 4.6 | 0 | 42 | 42 |
| 7_out | 5.3 | 0 | 42 | 42 |
| total | 76.3 | 83 | 250 | 750 |

### baseline (recorded pace x20, 1 runs, 0.02 M packets/s)

| stage | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| 1_queue | 835.9 | 0 | 6292 | 66083 |
| 2_parse | 14.1 | 0 | 83 | 3958 |
| 3_decode | 20.2 | 0 | 125 | 3625 |
| 4_apply | 214.5 | 83 | 1708 | 4792 |
| 5_top_of_book | 10.1 | 0 | 42 | 208 |
| 6_strategy | 5.7 | 0 | 42 | 42 |
| 7_out | 5.3 | 0 | 42 | 42 |
| total | 1105.8 | 125 | 7333 | 69917 |

### flat (recorded pace x20, 1 runs, 0.02 M packets/s)

| stage | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| 1_queue | 687.7 | 0 | 6292 | 88167 |
| 2_parse | 12.4 | 0 | 83 | 3500 |
| 3_decode | 22.5 | 0 | 125 | 4458 |
| 4_apply | 184.5 | 83 | 1875 | 4625 |
| 5_top_of_book | 11.6 | 0 | 42 | 333 |
| 6_strategy | 4.8 | 0 | 42 | 42 |
| 7_out | 5.6 | 0 | 42 | 42 |
| total | 929.1 | 83 | 7375 | 91250 |

### hash (recorded pace x20, 1 runs, 0.02 M packets/s)

| stage | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| 1_queue | 976.8 | 0 | 1417 | 17292 |
| 2_parse | 8.0 | 0 | 42 | 1750 |
| 3_decode | 14.6 | 0 | 83 | 1750 |
| 4_apply | 72.6 | 42 | 458 | 1125 |
| 5_top_of_book | 8.1 | 0 | 42 | 125 |
| 6_strategy | 3.2 | 0 | 42 | 42 |
| 7_out | 3.9 | 0 | 42 | 42 |
| total | 1087.2 | 42 | 2625 | 19000 |

### pool (recorded pace x20, 1 runs, 0.02 M packets/s)

| stage | mean ns | p50 | p99 | p99.9 |
|---|---|---|---|---|
| 1_queue | 241.9 | 0 | 3417 | 21750 |
| 2_parse | 13.1 | 0 | 42 | 3917 |
| 3_decode | 21.8 | 0 | 83 | 4292 |
| 4_apply | 109.6 | 42 | 667 | 3417 |
| 5_top_of_book | 9.3 | 0 | 42 | 167 |
| 6_strategy | 4.6 | 0 | 42 | 42 |
| 7_out | 5.7 | 0 | 42 | 42 |
| total | 405.9 | 83 | 4542 | 24708 |

