#!/usr/bin/env python3
import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


def load_rows(csv_path: Path):
    with csv_path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    if not rows:
        raise RuntimeError(f"no rows found in {csv_path}")
    return rows


def to_float(row, key, default=0.0):
    v = row.get(key, "")
    if v is None or v == "":
        return default
    return float(v)


def pick_cmd(rows, cmd):
    if cmd:
        filtered = [r for r in rows if str(r.get("cmd", "")).upper() == cmd.upper()]
        if not filtered:
            raise RuntimeError(f"no rows found for cmd={cmd}")
        return filtered
    return rows


def aggregate(rows):
    grouped = defaultdict(list)
    for row in rows:
        backend = row.get("backend", "unknown")
        grouped[backend].append(row)

    backends = []
    qps = []
    vmrss = []
    vmsize = []
    mem_gap = []
    fallback = []

    for backend in sorted(grouped.keys()):
        items = grouped[backend]
        backends.append(backend)
        qps.append(sum(to_float(r, "qps") for r in items) / len(items))
        vmrss.append(sum(to_float(r, "vmrss_kb") for r in items) / len(items))
        vmsize.append(sum(to_float(r, "vmsize_kb") for r in items) / len(items))
        mem_gap.append(sum(to_float(r, "mem_gap_kb") for r in items) / len(items))
        fallback.append(sum(to_float(r, "fallback_alloc_calls") for r in items) / len(items))

    return {
        "backends": backends,
        "qps": qps,
        "vmrss_kb": vmrss,
        "vmsize_kb": vmsize,
        "mem_gap_kb": mem_gap,
        "fallback_alloc_calls": fallback,
    }


def add_bar_labels(ax, values):
    ymax = max(values) if values else 0
    offset = ymax * 0.01 if ymax > 0 else 0.1
    for i, v in enumerate(values):
        label = f"{v:.2f}" if abs(v) < 10000 else f"{v:.0f}"
        ax.text(i, v + offset, label, ha="center", va="bottom", fontsize=9)


def plot_metric(backends, values, ylabel, title, out_path: Path):
    plt.figure(figsize=(8, 5))
    plt.bar(backends, values)
    plt.ylabel(ylabel)
    plt.title(title)
    add_bar_labels(plt.gca(), values)
    plt.tight_layout()
    plt.savefig(out_path, dpi=160, bbox_inches="tight")
    plt.close()


def write_summary(out_dir: Path, cmd: str, agg: dict):
    summary_path = out_dir / "bench_plot_summary.txt"
    with summary_path.open("w", encoding="utf-8") as f:
        f.write(f"cmd={cmd or 'ALL'}\n")
        for i, backend in enumerate(agg["backends"]):
            f.write(
                f"{backend}: "
                f"qps={agg['qps'][i]:.2f}, "
                f"vmrss_kb={agg['vmrss_kb'][i]:.2f}, "
                f"vmsize_kb={agg['vmsize_kb'][i]:.2f}, "
                f"mem_gap_kb={agg['mem_gap_kb'][i]:.2f}, "
                f"fallback_alloc_calls={agg['fallback_alloc_calls'][i]:.2f}\n"
            )


def main():
    parser = argparse.ArgumentParser(description="Plot kvstore benchmark results from bench_results.csv")
    parser.add_argument("--csv", default="bench_results.csv", help="input csv path")
    parser.add_argument("--outdir", default="bench_plots", help="output directory")
    parser.add_argument("--cmd", default="", help="only plot rows matching this command, e.g. HSET")
    args = parser.parse_args()

    csv_path = Path(args.csv)
    out_dir = Path(args.outdir)
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(csv_path)
    rows = pick_cmd(rows, args.cmd)
    agg = aggregate(rows)

    if not agg["backends"]:
        raise RuntimeError("no benchmark rows to plot")

    suffix = args.cmd.upper() if args.cmd else "ALL"

    plot_metric(
        agg["backends"],
        agg["qps"],
        "QPS",
        f"Benchmark QPS ({suffix})",
        out_dir / f"qps_{suffix}.png",
    )
    plot_metric(
        agg["backends"],
        agg["vmrss_kb"],
        "VmRSS (KB)",
        f"Benchmark VmRSS ({suffix})",
        out_dir / f"vmrss_{suffix}.png",
    )
    plot_metric(
        agg["backends"],
        agg["vmsize_kb"],
        "VmSize (KB)",
        f"Benchmark VmSize ({suffix})",
        out_dir / f"vmsize_{suffix}.png",
    )
    plot_metric(
        agg["backends"],
        agg["mem_gap_kb"],
        "VmSize - VmRSS (KB)",
        f"Benchmark Memory Gap ({suffix})",
        out_dir / f"memgap_{suffix}.png",
    )

    if any(v != 0 for v in agg["fallback_alloc_calls"]):
        plot_metric(
            agg["backends"],
            agg["fallback_alloc_calls"],
            "fallback_alloc_calls",
            f"Custom Pool Fallback Calls ({suffix})",
            out_dir / f"fallback_{suffix}.png",
        )

    write_summary(out_dir, args.cmd, agg)

    print(f"wrote plots to {out_dir.resolve()}")
    for p in sorted(out_dir.iterdir()):
        print(p.resolve())


if __name__ == "__main__":
    raise SystemExit(main())
