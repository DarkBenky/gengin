#!/usr/bin/env bash
# Perf-sample a binary and render the profile charts (flame graph, icicle, call
# graph) plus the raw perf.data next to <out.svg>.
#
#   tools/flame.sh build/tests/AOBench_flame build/tests/flame/AOBench.svg
#   tools/flame.sh build/main/main_bench_flame build/prof/bench.svg
#
# Unprivileged perf is blocked on machines with perf_event_paranoid >= 3, so
# fall back to passwordless sudo and otherwise just say how to unblock it. A
# missing graph must never fail the caller: test/bench results stay authoritative.
set -u

bin=${1:-}
svg=${2:-}
seconds=${3:-}
if [[ -z $bin || -z $svg ]]; then
	echo "usage: $0 <binary> <out.svg> [seconds]" >&2
	exit 1
fi
if [[ ! -x $bin ]]; then
	echo "[flame] no binary: $bin" >&2
	exit 1
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fgdir=$root/.flamegraph
name=$(basename "$bin")
name=${name%_flame}
data=${svg%.svg}.perf.data
base=${svg%.svg}
icicle=$base.icicle.svg
callgraph=$base.callgraph.svg
work=$base.work
trap 'rm -rf "$work"' EXIT
mkdir -p "$work" "$(dirname "$svg")"

if [[ ! -f $fgdir/flamegraph.pl || ! -f $fgdir/stackcollapse-perf.pl ]]; then
	echo "[flame] fetching FlameGraph tools into $fgdir"
	if ! git clone --depth=1 https://github.com/brendangregg/FlameGraph "$fgdir" >/dev/null; then
		echo "[flame] clone failed - skipping"
		exit 0
	fi
fi

if ! command -v perf >/dev/null; then
	echo "[flame] perf not installed - skipping"
	exit 0
fi

# 999 Hz keeps the sampling overhead small enough not to distort the run
hz=999
child=("$bin")
[[ -n $seconds ]] && child=(timeout "$seconds" "$bin")

rm -f "$data"
if ! perf record -F "$hz" -g --call-graph fp -o "$data" -- "${child[@]}" >/dev/null 2>"$data.err" && [[ ! -s $data ]]; then
	if ! sudo -n true 2>/dev/null; then
		echo "[flame] no graph for $name: perf counters are blocked"
		echo "[flame]   perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo '?')"
		echo "[flame]   enable once: bash llmOpt/scripts/enable-perf.sh   (or: sudo sysctl kernel.perf_event_paranoid=1)"
		rm -f "$data" "$data.err"
		exit 0
	fi

	rm -f "$data"
	if ! sudo perf record -F "$hz" -g --call-graph fp -o "$data" -- "${child[@]}" >/dev/null 2>"$data.err" && [[ ! -s $data ]]; then
		echo "[flame] perf record failed:"
		sed 's/^/    /' "$data.err" | head -4
		rm -f "$data" "$data.err"
		exit 0
	fi
	# root-owned perf.data is unreadable for the flamegraph pipeline
	sudo chown "$USER" "$data" 2>/dev/null || true
fi
rm -f "$data.err"

perf script -i "$data" > "$work/script" 2>/dev/null
perl "$fgdir/stackcollapse-perf.pl" < "$work/script" > "$work/folded"

# folded weights are event periods (cycles at -F), so count sample records for the report
samples=$(grep -c '^[^[:space:]]' "$work/script")
if [[ $samples -eq 0 ]]; then
	echo "[flame] no samples collected for $name - skipping"
	exit 0
fi

perl "$fgdir/flamegraph.pl" --title "$name" --width 1800 < "$work/folded" > "$svg"
perl "$fgdir/flamegraph.pl" --inverted --title "$name (icicle)" --width 1800 < "$work/folded" > "$icicle"

# call graph: same samples, but laid out as a tree of functions instead of stacks
gprof2dot_cmd=""
if command -v gprof2dot >/dev/null 2>&1; then
	gprof2dot_cmd="gprof2dot"
elif python3 -m gprof2dot --help >/dev/null 2>&1; then
	gprof2dot_cmd="python3 -m gprof2dot"
fi

if [[ -n $gprof2dot_cmd ]] && command -v dot >/dev/null 2>&1; then
	$gprof2dot_cmd -f perf < "$work/script" 2>/dev/null | dot -Tsvg -o "$callgraph"
	[[ -s $callgraph ]] || echo "[flame] call graph render failed - skipping"
else
	echo "[flame] call graph skipped: pip install gprof2dot && conda install -c conda-forge graphviz"
fi

echo "[flame] $name: $samples samples"
echo "[flame]   flame graph -> $svg"
echo "[flame]   icicle      -> $icicle"
[[ -s $callgraph ]] && echo "[flame]   call graph  -> $callgraph"
echo "[flame]   raw samples -> $data (perf report -i)"
