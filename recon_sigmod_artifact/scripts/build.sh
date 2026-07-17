#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build" "$ROOT/results"

CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--O3 -std=c++17 -pthread}"

"$CXX" $CXXFLAGS "$ROOT/src/cbcn.cpp" -o "$ROOT/build/cbcn"
"$CXX" $CXXFLAGS "$ROOT/src/downstream_tasks_bench.cpp" -o "$ROOT/build/downstream_tasks_bench"

echo "Built:"
echo "  $ROOT/build/cbcn"
echo "  $ROOT/build/downstream_tasks_bench"

