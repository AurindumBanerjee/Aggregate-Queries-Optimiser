| Workload | aqo | SQLite |
|---|---:|---:|
| Insert, sequential pk (rows/s) | 193,742 | 744,884 |
| Insert, random pk (rows/s) | 79,706 | 45,643 |
| Insert, sequential pk + aggregate indexes (rows/s) | 62,666 | 28,847 |
| Point lookup: aqo Table::get (no SQL) (ops/s, p50 탎) | 131,640 / 2.00 | - |
| Point lookup: aqo SQL (parse+plan each time) (ops/s, p50 탎) | 73,989 / 6.80 | - |
| Point lookup: sqlite prepared, autocommit reads (ops/s, p50 탎) | - | 100,024 / 8.50 |
| Point lookup: sqlite prepared, one open read txn (ops/s, p50 탎) | - | 541,244 / 1.50 |
| Range aggregate 0.01% of rows, p50 탎 | 2.7 | 17.7 |
| Range aggregate 0.1% of rows, p50 탎 | 7.2 | 151.0 |
| Range aggregate 1% of rows, p50 탎 | 14.9 | 1,472.7 |
| Range aggregate 10% of rows, p50 탎 | 16.9 | 14,465.9 |
| Range aggregate 50% of rows, p50 탎 | 30.2 | 70,719.2 |
