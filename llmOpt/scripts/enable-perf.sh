#!/usr/bin/env bash
# Enable unprivileged perf counters for run_perf_stat / make_flame.
#
# Ubuntu ships kernel.perf_event_paranoid=4, which blocks perf for non-root.
# This lowers it to 1 (persistent via /etc/sysctl.d) and falls back to granting
# CAP_PERFMON to the perf binary when the sysctl cannot be written.
#
#   enable-perf.sh --check    report status without changing anything
#   enable-perf.sh            apply (prompts for the sudo password once)
set -euo pipefail

SYSCTL_CONF=/etc/sysctl.d/99-gengin-perf.conf

perfWorks() {
  perf stat -e cycles true >/dev/null 2>&1
}

if [[ "${1:-}" == "--check" ]]; then
  paranoid="$(cat /proc/sys/kernel/perf_event_paranoid)"
  if perfWorks; then
    echo "perf counters: OK (perf_event_paranoid=$paranoid)"
    exit 0
  fi
  echo "perf counters: BLOCKED (perf_event_paranoid=$paranoid)"
  echo "  fix: $0"
  exit 1
fi

if perfWorks; then
  paranoid="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 0)"
  if [[ "$paranoid" -le 1 ]]; then
    echo "perf counters already work (perf_event_paranoid=$paranoid) — nothing to do"
    exit 0
  fi
  # perf works for THIS user (e.g. root) but the sysctl still blocks other
  # identities such as the supervisor service account.
  echo "perf works here but perf_event_paranoid=$paranoid blocks other users — lowering to 1"
fi

echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid) — requesting sudo to enable counters"
echo "kernel.perf_event_paranoid=1" | sudo tee "$SYSCTL_CONF" >/dev/null
sudo sysctl -w kernel.perf_event_paranoid=1 >/dev/null

if perfWorks; then
  echo "perf counters enabled (persistent via $SYSCTL_CONF)"
  exit 0
fi

perf_bin="$(readlink -f "$(command -v perf)")"
echo "sysctl did not unblock perf — trying cap_perfmon+ep on $perf_bin"
sudo setcap cap_perfmon+ep "$perf_bin"

if perfWorks; then
  echo "perf counters enabled via cap_perfmon on $perf_bin"
  echo "note: a perf package update replaces the binary and drops the capability"
  exit 0
fi

echo "error: perf still blocked — check 'dmesg | tail' and /proc/sys/kernel/perf_event_paranoid" >&2
exit 1
