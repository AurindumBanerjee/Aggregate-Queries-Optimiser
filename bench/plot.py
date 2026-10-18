"""Render bench/results/results.json into docs/*.png and a Markdown results table.

    python bench/plot.py [results.json]
"""
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
src = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "bench" / "results" / "results.json"
data = json.loads(src.read_text(encoding="utf-8"))
res = data["results"]
docs = ROOT / "docs"
docs.mkdir(exist_ok=True)

COLORS = {"aqo": "#2a6fdb", "sqlite": "#d9822b", "aqo-scan": "#8a8f98"}


def pick(workload, **kw):
    return [r for r in res if r["workload"] == workload and all(r.get(k) == v for k, v in kw.items())]


# ---- 1. range aggregates: latency vs selectivity (log-log) ----
fig, ax = plt.subplots(figsize=(7.5, 4.6))
series = [
    ("aqo", "aggregate index direct API", "aqo aggregate index (API)", COLORS["aqo"], "-o"),
    ("aqo", "aggregate index via SQL", "aqo aggregate index (SQL, parse each query)", COLORS["aqo"], "--s"),
    ("sqlite", "covering index (a,b,c)", "SQLite covering index", COLORS["sqlite"], "-^"),
    ("aqo", "full scan (index disabled)", "aqo full scan", COLORS["aqo-scan"], ":x"),
]
for eng, var, label, color, style in series:
    pts = sorted((r["selectivity"], r["p50_us"]) for r in pick("range_aggregate", engine=eng, variant=var))
    if pts:
        ax.plot([p[0] * 100 for p in pts], [p[1] for p in pts], style, color=color, label=label, markersize=5)
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("range selectivity (% of rows matched)")
ax.set_ylabel("median query latency (µs)")
ax.set_title(f"SUM/COUNT/MAX over a range, {data['config']['rows']:,} rows")
ax.grid(True, which="both", alpha=0.25)
ax.legend(fontsize=8)
fig.tight_layout()
fig.savefig(docs / "bench_range_aggregates.png", dpi=140)

# ---- 2. insert throughput and point lookups ----
fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.4))
variants = ["sequential pk", "random pk", "sequential pk + aggregate indexes"]
w = 0.38
for i, eng in enumerate(["aqo", "sqlite"]):
    vals = [next((r["rows_per_sec"] for r in pick("insert", engine=eng, variant=v)), 0) / 1000 for v in variants]
    a1.bar([x + (i - 0.5) * w for x in range(3)], vals, w, label=eng, color=COLORS[eng])
a1.set_xticks(range(3))
a1.set_xticklabels(["sequential\nkeys", "random\nkeys", "sequential +\n2 indexes"], fontsize=8)
a1.set_ylabel("rows / second (thousands)")
a1.set_title("Bulk insert, fsync per 10k-row commit")
a1.legend()
lk = [(r["engine"] + ": " + r["variant"], r["ops_per_sec"] / 1000, r["engine"]) for r in pick("point_lookup")]
a2.barh(range(len(lk)), [x[1] for x in lk], color=[COLORS[x[2]] for x in lk])
a2.set_yticks(range(len(lk)))
a2.set_yticklabels([x[0] for x in lk], fontsize=7)
a2.set_xlabel("lookups / second (thousands)")
a2.set_title("Random point lookups by primary key")
fig.tight_layout()
fig.savefig(docs / "bench_insert_lookup.png", dpi=140)

# ---- Markdown table ----
rows = []


def fmt(x, digits=1):
    return f"{x:,.{digits}f}"


for v in variants:
    a = next(iter(pick("insert", engine="aqo", variant=v)), None)
    s = next(iter(pick("insert", engine="sqlite", variant=v)), None)
    if a and s:
        rows.append((f"Insert, {v} (rows/s)", fmt(a["rows_per_sec"], 0), fmt(s["rows_per_sec"], 0)))
for r in pick("point_lookup"):
    rows.append((f"Point lookup: {r['engine']} {r['variant']} (ops/s, p50 µs)", f"{fmt(r['ops_per_sec'], 0)} / {r['p50_us']:.2f}" if r["engine"] == "aqo" else "-",
                 f"{fmt(r['ops_per_sec'], 0)} / {r['p50_us']:.2f}" if r["engine"] == "sqlite" else "-"))
for sel in sorted({r["selectivity"] for r in pick("range_aggregate")}):
    a = next(iter(pick("range_aggregate", engine="aqo", variant="aggregate index direct API", selectivity=sel)), None)
    s = next(iter(pick("range_aggregate", engine="sqlite", variant="covering index (a,b,c)", selectivity=sel)), None)
    if a and s:
        rows.append((f"Range aggregate {sel*100:g}% of rows, p50 µs", fmt(a["p50_us"], 1), fmt(s["p50_us"], 1)))
md = ["| Workload | aqo | SQLite |", "|---|---:|---:|"] + [f"| {a} | {b} | {c} |" for a, b, c in rows]
(ROOT / "bench" / "results" / "table.md").write_text("\n".join(md) + "\n")
print("\n".join(md))
