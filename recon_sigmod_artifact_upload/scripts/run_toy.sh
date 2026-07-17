#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ ! -x "$ROOT/build/recon" ]]; then
  bash "$ROOT/scripts/build.sh"
fi

mkdir -p "$ROOT/results"

"$ROOT/build/recon" \
  "$ROOT/data/toy/toy_graph.mtx" \
  "$ROOT/results/toy_graph.recon.bin" \
  64 \
  2 \
  4096 \
  4 \
  1 \
  0

"$ROOT/build/downstream_tasks_bench" \
  "$ROOT/data/toy/toy_graph.mtx" \
  "$ROOT/results/toy_graph.recon.bin" \
  1

