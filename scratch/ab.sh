#!/bin/bash
# Interleaved paired A/B: alternate base/mod within each round and print per-round
# medians.  Host drift cancels out; the order flips every round.
set -e
cd /root/gengin/llmOpt/gengin
ROUNDS=${1:-10}
OUT=${2:-scratch/ab_results.txt}
: > "$OUT"
run() {
  local v=$1
  local line
  line=$(./build/ab/${v}_bench 2>/dev/null | grep -E '^  (avg|median|p99)' | awk '{print $3}' | tr '\n' ' ')
  echo "$v $line" >> "$OUT"
  echo "$v $line"
}
for i in $(seq 1 "$ROUNDS"); do
  echo "--- round $i"
  if [ $((i % 2)) -eq 1 ]; then run base; run mod; else run mod; run base; fi
done
