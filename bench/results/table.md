| Workload | aqo | SQLite |
|---|---:|---:|
| Insert, sequential pk (rows/s) | 193,742 (142,058–203,785) | 744,884 (304,791–855,846) |
| Insert, random pk (rows/s) | 79,706 (57,871–108,958) | 40,525 (26,665–45,643) |
| Insert, sequential pk + aggregate indexes (rows/s) | 42,047 (29,760–62,666) | 18,424 (17,048–28,847) |
| Point lookup: aqo Table::get (no SQL) (ops/s, p50 µs) | 131,640 / 2.00 | - |
| Point lookup: aqo SQL (parse+plan each time) (ops/s, p50 µs) | 73,989 / 6.80 | - |
| Point lookup: sqlite prepared, autocommit reads (ops/s, p50 µs) | - | 100,024 / 8.50 |
| Point lookup: sqlite prepared, one open read txn (ops/s, p50 µs) | - | 541,244 / 1.50 |
| Range aggregate 0.01% of rows, p50 µs | 4.2 (2.7–9.7) | 22.0 (17.7–67.1) |
| Range aggregate 0.1% of rows, p50 µs | 8.1 (7.2–22.6) | 151.0 (140.3–571.7) |
| Range aggregate 1% of rows, p50 µs | 14.9 (13.4–22.8) | 1,472.7 (1,353.5–2,546.7) |
| Range aggregate 10% of rows, p50 µs | 17.1 (16.9–29.8) | 14,465.9 (13,810.3–26,681.9) |
| Range aggregate 50% of rows, p50 µs | 30.8 (30.2–50.7) | 81,485.7 (70,719.2–118,821.0) |
