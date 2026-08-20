"""Turn the raw benchmark JSON into (a) a summary table and (b) the JS payload
the report page plots. Medians throughout: with 4 runs on a busy VM the mean is
hostage to one outlier, and the median is what a user experiences."""
import json, io, statistics, sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "bench.json"
OUT = sys.argv[2] if len(sys.argv) > 2 else "report-data.json"
rows = json.load(io.open(SRC, encoding="utf-8-sig"))

by = {}
for r in rows:
    by.setdefault(r["label"], []).append(r)

def med(xs):
    return statistics.median(xs)

summary = {}
for label, rs in by.items():
    summary[label] = {
        "n": len(rs),
        "settledMs": med([r["tSettledMs"] for r in rs]),
        "contentMs": med([r["tContentMs"] for r in rs]),
        "windowMs":  med([r["tWindowMs"]  for r in rs]),
        "memMB":     med([r["memMB"]      for r in rs]),
        "privMB":    med([r.get("privMB", 0) for r in rs]),
        "procs":     med([r["procs"]      for r in rs]),
        "idleCpu":   med([r["idleCpuPct"] for r in rs]),
        "win":       "%dx%d" % (rs[0]["winW"], rs[0]["winH"]),
        # spread, so the report can be honest about run-to-run variation
        "settledMin": min(r["tSettledMs"] for r in rs),
        "settledMax": max(r["tSettledMs"] for r in rs),
    }

print("%-20s %4s %9s %9s %8s %8s %6s %8s" %
      ("label", "n", "settled", "content", "wsMB", "privMB", "procs", "idleCPU"))
for k in sorted(summary):
    s = summary[k]
    print("%-20s %4d %9d %9d %8.1f %8.1f %6d %8.2f" %
          (k, s["n"], s["settledMs"], s["contentMs"], s["memMB"],
           s["privMB"], s["procs"], s["idleCpu"]))

# the representative paint trace for each app: the run whose settle time is the
# median one, so the plotted curve is the same run the headline number came from
traces = {}
for label in ("unocode", "vscode-clean", "vscode-real"):
    if label not in by:
        continue
    rs = sorted(by[label], key=lambda r: r["tSettledMs"])
    pick = rs[len(rs) // 2]
    traces[label] = {
        "points": [[p[0], p[1]] for p in pick["trace"]],
        "settled": pick["tSettledMs"],
        "content": pick["tContentMs"],
    }

out = {"summary": summary, "traces": traces}
io.open(OUT, "w", encoding="utf-8").write(json.dumps(out))
print("\nwrote report-data.json")
for k, t in traces.items():
    print("  trace %-14s %4d samples, settle %d ms" % (k, len(t["points"]), t["settled"]))
