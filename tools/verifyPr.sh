#!/usr/bin/env bash
# Apply a pull request's C changes to a clean checkout, build, run the gate its
# category is measured with, then restore the tree. The gate prints the same
# lines the PR body quotes, so the two can be compared directly.
#
#   tools/verifyPr.sh 141 flight
#   tools/verifyPr.sh 121 render 5
#
# Only tracked *.c/*.h/*.cl hunks are applied, so screenshots or asset binaries
# in the PR cannot stop the source from being tested. The render gate is
# repeated because one run is inside the frame-time noise; the flight suite is
# deterministic apart from its controller cost.
set -euo pipefail

pr=${1:-}
gate=${2:-render}
runs=${3:-3}
if [[ -z $pr ]]; then
	echo "usage: $0 <pr-number> [render|flight] [runs]" >&2
	exit 1
fi
if [[ $gate != render && $gate != flight ]]; then
	echo "[verifyPr] unknown gate: $gate (render or flight)" >&2
	exit 1
fi
if [[ $gate == flight ]]; then
	runs=1
fi

cd "$(git rev-parse --show-toplevel)"
echo "[verifyPr] fetching pull/$pr/head"
git fetch --quiet origin "pull/$pr/head"
head=$(git rev-parse FETCH_HEAD)
base=$(git merge-base HEAD "$head")

# Only the files this PR changes are ever touched, so unrelated local edits and
# generated artifacts (logs, flame graphs) do not block a run and cannot be lost.
touched=$(git diff --name-only "$base" "$head" -- '*.c' '*.h' '*.cl')
if [[ -z $touched ]]; then
	echo "[verifyPr] pull/$pr changes no C source - nothing to test" >&2
	exit 1
fi
existing=$(git status --porcelain --untracked-files=no -- $touched)
if [[ -n $existing ]]; then
	echo "[verifyPr] local changes in files this PR touches:" >&2
	echo "$existing" >&2
	exit 1
fi

echo "[verifyPr] pull/$pr head ${head:0:8} on base ${base:0:8}"
git diff --stat "$base" "$head" -- '*.c' '*.h' '*.cl' | tail -n 1

patch=$(mktemp)
added=$(git diff --name-only --diff-filter=A "$base" "$head" -- '*.c' '*.h' '*.cl')
restore() {
	git reset -q -- $touched
	local tracked
	tracked=$(git ls-files -- $touched)
	if [[ -n $tracked ]]; then
		git checkout -- $tracked
	fi
	if [[ -n $added ]]; then
		rm -f $added
	fi
	rm -f "$patch"
}
trap restore EXIT

git diff "$base" "$head" -- '*.c' '*.h' '*.cl' > "$patch"
if ! git apply -3 "$patch"; then
	echo "[verifyPr] patch does not apply - conflicting files:" >&2
	git diff --name-only --diff-filter=U >&2
	exit 1
fi

for ((run = 1; run <= runs; run++)); do
	if [[ $runs -gt 1 ]]; then
		echo "[verifyPr] $gate run $run/$runs"
	fi
	if [[ $gate == flight ]]; then
		make flightBench
	else
		make bench
	fi
done

echo
echo "[verifyPr] done - compare the numbers above with pull/$pr's body"
