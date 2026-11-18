### 1 day (2025-04-07 open), baseline — recorded 2 k pkt/s on average, busiest ms 111 packets

| speed | offered avg | offered peak | total p50 | p99 | p99.9 | max | queue p99 |
|---|---|---|---|---|---|---|---|
| x10 | 17 k | 1.11 M | 125 ns | 4625 ns | 68.6 µs | 5.3 ms | 3333 ns |
| x30 | 51 k | 3.33 M | 125 ns | 11.9 µs | 5.4 ms | 12.5 ms | 10.8 µs |
| x100 | 171 k | 11.10 M | 125 ns | 4375 ns | 16.5 µs | 127.9 µs | 4125 ns |
| x300 | 514 k | 33.30 M | 167 ns | 10.7 µs | 25.6 µs | 110.2 µs | 10.5 µs |
| x1000 | 1.71 M | 111.00 M | 583 ns | 40.6 µs | 81.6 µs | 118.2 µs | 40.5 µs |
| x3000 | 5.14 M | 333.00 M | 5083 ns | 410.4 µs | 572.2 µs | 589.1 µs | 410.4 µs |
| back to back | 8.67 M | — | 83 ns | 458 ns | 1250 ns | 60.8 µs | 42 ns |

### 1 day (2025-04-07 open), hash — recorded 2 k pkt/s on average, busiest ms 111 packets

| speed | offered avg | offered peak | total p50 | p99 | p99.9 | max | queue p99 |
|---|---|---|---|---|---|---|---|
| x10 | 17 k | 1.11 M | 83 ns | 3333 ns | 22.3 µs | 885.9 µs | 2125 ns |
| x30 | 51 k | 3.33 M | 42 ns | 2042 ns | 13.0 µs | 2.1 ms | 1250 ns |
| x100 | 171 k | 11.10 M | 42 ns | 3125 ns | 155.9 µs | 1.8 ms | 2875 ns |
| x300 | 514 k | 33.30 M | 83 ns | 5333 ns | 45.4 µs | 130.2 µs | 5250 ns |
| x1000 | 1.71 M | 111.00 M | 250 ns | 18.9 µs | 56.4 µs | 123.0 µs | 18.8 µs |
| x3000 | 5.14 M | 333.00 M | 1292 ns | 127.4 µs | 195.4 µs | 202.8 µs | 127.3 µs |
| back to back | 12.78 M | — | 42 ns | 208 ns | 542 ns | 39.3 µs | 42 ns |

### 3 days merged, baseline — recorded 4 k pkt/s on average, busiest ms 131 packets

| speed | offered avg | offered peak | total p50 | p99 | p99.9 | max | queue p99 |
|---|---|---|---|---|---|---|---|
| x10 | 39 k | 1.31 M | 125 ns | 5708 ns | 87.2 µs | 11.4 ms | 4500 ns |
| x30 | 118 k | 3.93 M | 125 ns | 4542 ns | 49.5 µs | 1.2 ms | 3917 ns |
| x100 | 394 k | 13.10 M | 125 ns | 8208 ns | 90.2 µs | 1.3 ms | 7958 ns |
| x300 | 1.18 M | 39.30 M | 292 ns | 35.0 µs | 141.3 µs | 188.0 µs | 34.7 µs |
| x1000 | 3.94 M | 131.00 M | 2917 ns | 768.6 µs | 1.1 ms | 1.2 ms | 768.5 µs |
| x3000 | 11.82 M | 393.00 M | 58.4 ms | 119.8 ms | 120.7 ms | 120.7 ms | 119.8 ms |
| back to back | 5.66 M | — | 125 ns | 750 ns | 2417 ns | 8.1 ms | 42 ns |

### 3 days merged, hash — recorded 4 k pkt/s on average, busiest ms 131 packets

| speed | offered avg | offered peak | total p50 | p99 | p99.9 | max | queue p99 |
|---|---|---|---|---|---|---|---|
| x10 | 39 k | 1.31 M | 83 ns | 3042 ns | 21.2 µs | 2.3 ms | 1333 ns |
| x30 | 118 k | 3.93 M | 83 ns | 2208 ns | 27.8 µs | 2.7 ms | 1458 ns |
| x100 | 394 k | 13.10 M | 83 ns | 11.2 µs | 117.5 µs | 1.5 ms | 10.8 µs |
| x300 | 1.18 M | 39.30 M | 125 ns | 37.2 µs | 1.1 ms | 1.5 ms | 36.8 µs |
| x1000 | 3.94 M | 131.00 M | 542 ns | 243.5 µs | 526.5 µs | 542.4 µs | 243.4 µs |
| x3000 | 11.82 M | 393.00 M | 1.6 ms | 7.3 ms | 7.4 ms | 7.5 ms | 7.3 ms |
| back to back | 11.78 M | — | 83 ns | 250 ns | 625 ns | 110.7 µs | 42 ns |

