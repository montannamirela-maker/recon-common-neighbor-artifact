# Recon Artifact

This repository contains an anonymized research artifact for the submitted
paper:

**Recon: A Common-Neighbor-Based Executable Representation for Graph
Analytics**.

Recon is a graph-compression prototype that reuses exact common-neighbor
overlap while retaining a lightweight executable representation for downstream
graph analytics. The artifact includes the compression prototype, downstream
benchmark code, a small sanity-check graph, processed result summaries used in
the paper, and instructions for obtaining the public datasets. It is prepared
for double-anonymous review.

## Repository Layout

```text
src/
  cbcn.cpp                       Recon compression prototype
  downstream_tasks_bench.cpp      downstream task benchmark prototype
scripts/
  build.sh                       compile the C++ prototypes
  run_toy.sh                     run a toy end-to-end compression check
  plot_summary.py                regenerate simple summary plots from CSV results
data/
  toy/toy_graph.mtx              tiny Matrix Market graph for smoke testing
results/
  compression_ratio_comparison.csv
  ablation_stage_summary.csv
  downstream_ratios.csv
docs/
  index.html                     noindex landing page for optional GitHub Pages use
DATA.md                          public dataset and preprocessing notes
ANONYMITY.md                     anonymity checklist
```

Some executable and intermediate file names still use `cbcn` for compatibility
with the submitted prototype scripts. In the paper and in this README, the
method is referred to as **Recon**.

## Requirements

The prototype uses standard C++17 and Python 3.

Recommended environment:

- Linux x86-64
- `g++` or `clang++` with C++17 support
- Python 3.9+
- Optional: `matplotlib` for `scripts/plot_summary.py`

The paper experiments were run on a Linux server with dual Intel Xeon E5-2696
v4 processors, 88 hardware threads, and 123 GB RAM. The toy example below is
small and should run on a laptop.

## Quick Start

Compile the artifact:

```bash
bash scripts/build.sh
```

Run the small end-to-end example:

```bash
bash scripts/run_toy.sh
```

Expected behavior:

- `build/cbcn` is compiled.
- `results/toy_graph.cbcn.bin` is generated.
- The output prints compression statistics and reports verification as
  `PASSED`.

## Running Recon on a Graph

Input graphs can be provided as Matrix Market files or plain edge-list files.
The compressor accepts positional arguments:

```bash
build/cbcn INPUT_PATH OUTPUT_PATH PARTITION_LIMIT MIN_COMMON MAX_POSTINGS THREADS VERIFY DISABLE_RESIDUAL_STITCHING
```

Example:

```bash
build/cbcn data/toy/toy_graph.mtx results/toy_graph.cbcn.bin 64 2 4096 4 1 0
```

Argument notes:

- `PARTITION_LIMIT`: maximum local partition size; use `0` for auto-tuning.
- `MIN_COMMON`: minimum exact common-neighbor overlap for pair capture.
- `MAX_POSTINGS`: cap for neighbor posting lists; use `0` for auto-tuning.
- `THREADS`: number of worker threads.
- `VERIFY`: `1` checks exact reconstruction against the input graph.
- `DISABLE_RESIDUAL_STITCHING`: `1` disables the final residual stitching stage.

## Downstream Benchmark

After generating a compressed graph:

```bash
build/downstream_tasks_bench data/toy/toy_graph.mtx results/toy_graph.cbcn.bin 1
```

The benchmark reports original and compressed execution paths for BFS,
connected components, k-core, PageRank, and triangle counting when the graph is
small enough for exact validation.

## Reproducing Paper Tables and Figures

The `results/` directory contains processed CSV summaries used to produce the
paper's compression, ablation, and downstream-ratio figures. To generate simple
summary plots:

```bash
python3 scripts/plot_summary.py
```

The full large-graph experiments require downloading the public datasets listed
in `DATA.md` and may take substantial time and memory.

## Dataset Notes

This artifact does not redistribute large third-party graph datasets. `DATA.md`
lists the public dataset sources and preprocessing assumptions used by the
experiments.

## Anonymity

This artifact is prepared for double-anonymous review. It intentionally avoids
author names, institution names, personal paths, private URLs, and tracking
links. See `ANONYMITY.md` for the checklist used before upload.
