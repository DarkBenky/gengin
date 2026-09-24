#!/bin/bash
# Interleaved paired A/B between two build/ab binaries, alternating order each
# round so host drift cancels.  Usage: ab_pair.sh <rounds> <binA> <binB> <out>
set -e
cd /root/gengin/llmOpt/gengin
ROUNDS=${1:-8}
A=${2:-base_sah_bench}
B=${3:-mod_sky_bench}
OUT=${4:-scratch/ab_pair.txt}
: > "$OUT"
run() {
  local v=$1
  local line
  line=$(./build/ab/${v} 2>/dev/null | grep -E '^  (avg|median|p99)' | awk '{print $3}' | tr '\n' ' ')
  echo "$v $line" >> "$OUT"
}
for i in $(seq 1 "$ROUNDS"); do
  if [ $((i % 2)) -eq 1 ]; then run "$A"; run "$B"; else run "$B"; run "$A"; fi
done
awk '{n[$1]++; a[$1]+=$2; m[$1]+=$3; p[$1]+=$4} END {for (k in n) printf "%-16s avg=%.2f median=%.2f p99=%.2f (n=%d)\n", k, a[k]/n[k], m[k]/n[k], p[k]/n[k], n[k]}' "$OUT"
awk '{n[$1]++; a[$1]+=$2; m[$1]+=$3} END {for (k in n) printf "%s %.4f %.4f\n", k, a[k]/n[k], m[k]/n[k]}' "$OUT" > "$OUT.agg"
