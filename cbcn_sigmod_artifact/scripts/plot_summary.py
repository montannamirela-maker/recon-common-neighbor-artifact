#!/usr/bin/env python3
from pathlib import Path
import csv

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
FIGURES = ROOT / "figures"
FIGURES.mkdir(exist_ok=True)

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except Exception as exc:
    raise SystemExit(
        "matplotlib is required for plotting. Install it or inspect the CSV files directly."
    ) from exc


def read_csv(path):
    with path.open(newline="") as fh:
        return list(csv.DictReader(fh))


def plot_compression():
    rows = [
        r for r in read_csv(RESULTS / "compression_ratio_comparison.csv")
        if r.get("Code_status") == "OK"
    ]
    selected = rows[:12]
    labels = [r["dataset"].replace("web-", "") for r in selected]
    values = [float(r["Code_chain_over_2E"]) for r in selected]

    fig, ax = plt.subplots(figsize=(10, 3.5))
    ax.bar(labels, values, color="white", edgecolor="#333333", hatch="///")
    ax.set_ylabel("CBCN chain / 2|E|")
    ax.set_title("CBCN compression summary from processed results")
    ax.tick_params(axis="x", rotation=45)
    ax.grid(axis="y", color="#dddddd", linewidth=0.6)
    fig.tight_layout()
    fig.savefig(FIGURES / "compression_summary.pdf")
    fig.savefig(FIGURES / "compression_summary.png", dpi=300)


def plot_ablation():
    rows = read_csv(RESULTS / "ablation_stage_summary.csv")
    labels = [r["dataset"].replace("web-", "") for r in rows]
    sg = [float(r["sg_over_cbcn"]) for r in rows]
    fb = [float(r["fb_over_cbcn"]) for r in rows]

    x = range(len(rows))
    width = 0.38
    fig, ax = plt.subplots(figsize=(9, 3.5))
    ax.bar([i - width / 2 for i in x], sg, width, label="SG-CN / CBCN",
           color="white", edgecolor="#333333", hatch="///")
    ax.bar([i + width / 2 for i in x], fb, width, label="FB-CN / CBCN",
           color="white", edgecolor="#333333", hatch="\\\\\\")
    ax.axhline(1.0, color="#555555", linewidth=0.8)
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel("relative compressed size")
    ax.set_title("Stage-ablation summary from processed results")
    ax.legend(frameon=False)
    ax.grid(axis="y", color="#dddddd", linewidth=0.6)
    fig.tight_layout()
    fig.savefig(FIGURES / "ablation_summary.pdf")
    fig.savefig(FIGURES / "ablation_summary.png", dpi=300)


def main():
    plot_compression()
    plot_ablation()
    print(f"Wrote figures to {FIGURES}")


if __name__ == "__main__":
    main()

