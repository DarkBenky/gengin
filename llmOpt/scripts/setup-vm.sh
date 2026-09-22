#!/usr/bin/env bash
# gengin-llmopt VM provisioning.
#
# Idempotent setup for an Ubuntu 22.04/24.04 VM that runs the commit-triggered
# optimizer. Run as root:  sudo bash llmOpt/scripts/setup-vm.sh [options]
#
# Options:
#   --checkout DIR   path to the gengin checkout (default: /opt/gengin)
#   --inputs DIR     stable ignored-inputs dir (default: /var/lib/gengin-llmopt/inputs)
#   --venv DIR       python venv for the MCP server (default: /opt/gengin-llmopt/venv)
#   --skip-perf      do not attempt to enable unprivileged perf counters
#
# What it does:
#   - verifies the distro is a supported Ubuntu LTS
#   - installs build/display/OpenCL/perf tooling
#   - creates llmopt-supervisor and llmopt-agent Unix users
#   - installs a root-owned agent-launch helper + exact sudoers rule
#   - provisions GENGIN_INPUTS_DIR with a verified manifest
#   - creates the MCP venv
#   - installs the systemd units (substituting checkout/venv paths)
#   - enables unprivileged perf counters (unless --skip-perf)
#
# It seeds the single secrets file if absent; review it by hand:
#   sudo nano /etc/gengin-llmopt/secrets.env   # management key + GITHUB_TOKEN

set -euo pipefail

CHECKOUT=/opt/gengin
INPUTS=/var/lib/gengin-llmopt/inputs
VENV=/opt/gengin-llmopt/venv
SKIP_PERF=0
HELPER_DIR=/usr/local/lib/gengin-llmopt
HELPER="$HELPER_DIR/agent-launch"
SUDOERS=/etc/sudoers.d/gengin-llmopt
CRED_DIR=/etc/systemd/credentials/gengin-llmopt

while [[ $# -gt 0 ]]; do
  case "$1" in
    --checkout) CHECKOUT="$2"; shift 2 ;;
    --inputs)   INPUTS="$2"; shift 2 ;;
    --venv)     VENV="$2"; shift 2 ;;
    --skip-perf) SKIP_PERF=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

log() { echo "[setup-vm] $*"; }
die() { echo "[setup-vm] ERROR: $*" >&2; exit 1; }

# --- distro check -----------------------------------------------------------
[[ -f /etc/os-release ]] || die "/etc/os-release not found"
. /etc/os-release
[[ "${ID:-}" == "ubuntu" ]] || die "unsupported distro: ${ID:-unknown} (need ubuntu)"
case "${VERSION_ID:-}" in
  22.04|24.04) log "supported Ubuntu ${VERSION_ID}" ;;
  *) die "unsupported Ubuntu ${VERSION_ID} (need 22.04 or 24.04)" ;;
esac

[[ -d "$CHECKOUT" ]] || die "checkout not found: $CHECKOUT"
[[ -f "$CHECKOUT/llmOpt/requirements-mcp.txt" ]] || die "not a gengin checkout: $CHECKOUT"
log "provisioning checkout: $CHECKOUT"

# --- packages ---------------------------------------------------------------
log "installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
  build-essential clang lld make git rsync curl ca-certificates \
  python3 python3-venv python3-pip \
  clangd \
  xvfb x11-utils x11-xkb-utils libx11-dev libxrandr-dev libxkbcommon-dev \
  mesa-utils libgl1-mesa-dev libgl1-mesa-dri libgl1-mesa-glx \
  pocl-opencl-icd ocl-icd-opencl-dev clinfo \
  libjpeg-dev perl graphviz \
  || die "apt-get install failed"

# perf tools are kernel-versioned; best-effort (the generic wrapper usually works)
if ! command -v perf >/dev/null 2>&1; then
  apt-get install -y -qq linux-tools-generic "linux-tools-$(uname -r)" 2>/dev/null \
    || apt-get install -y -qq linux-tools-generic 2>/dev/null \
    || log "WARNING: perf not installed (install linux-tools-* manually for REQUIRE_PERF=true)"
fi

# gprof2dot is a python package; install into the system python for the helper.
python3 -m pip install --quiet gprof2dot 2>/dev/null || log "gprof2dot pip install skipped (will use conda/system if present)"

# --- users ------------------------------------------------------------------
log "creating users"
id -u llmopt-supervisor >/dev/null 2>&1 || useradd --system --create-home --shell /usr/sbin/nologin llmopt-supervisor
# The agent needs a real home: Hermes installs under ~/.hermes and its
# launcher lives in ~/.local/bin.
id -u llmopt-agent >/dev/null 2>&1 || useradd --system --create-home --shell /bin/bash llmopt-agent
# Shared group for the narrow handoff dirs the supervisor creates and the
# agent must read/write (per-session run dirs, session logs).
getent group gengin-llmopt >/dev/null 2>&1 || groupadd --system gengin-llmopt
usermod -aG gengin-llmopt llmopt-supervisor
usermod -aG gengin-llmopt llmopt-agent

# Files synced from a workstation may carry a foreign uid; with
# fs.protected_regular=2 even root cannot rewrite files it does not own inside
# sticky directories, so normalize ownership before anything else runs.
find "$CHECKOUT/llmOpt" -uid 1000 -exec chown llmopt-supervisor:gengin-llmopt {} + 2>/dev/null || true

# state: supervisor-owned (contains key hashes); agent has no access.
mkdir -p "$CHECKOUT/llmOpt/state" "$CHECKOUT/llmOpt/logs/sessions" "$CHECKOUT/llmOpt/run"
chown -R llmopt-supervisor:gengin-llmopt "$CHECKOUT/llmOpt/state"
chmod 2750 "$CHECKOUT/llmOpt/state"
# logs/run: supervisor-owned, group-writable (setgid) so the agent can write
# its per-session artifacts (usage.json, result.json, Hermes home).
chown -R llmopt-supervisor:gengin-llmopt "$CHECKOUT/llmOpt/logs" "$CHECKOUT/llmOpt/run"
chmod 2770 "$CHECKOUT/llmOpt/logs" "$CHECKOUT/llmOpt/logs/sessions" "$CHECKOUT/llmOpt/run"
# llmOpt/: the supervisor clones the sandbox into it (gengin.prepare-<id> ->
# gengin); the sticky bit stops the agent from renaming/removing entries it
# does not own (supervisor code, units, credentials). setgid keeps new entries
# in gengin-llmopt so the group-write model keeps working with UMask=0007.
chown llmopt-supervisor:gengin-llmopt "$CHECKOUT/llmOpt"
chmod 3770 "$CHECKOUT/llmOpt"
# The knowledge base is written by the agent during sessions; ownership by the
# agent UID keeps in-place rewrites legal under the sticky bit.
if [[ -f "$CHECKOUT/llmOpt/codebase_context.md" ]]; then
  chown llmopt-agent:gengin-llmopt "$CHECKOUT/llmOpt/codebase_context.md"
  chmod 0664 "$CHECKOUT/llmOpt/codebase_context.md"
fi
# The sandbox is agent-writable (group-shared, setgid for inherited group).
if [[ -d "$CHECKOUT/llmOpt/gengin" ]]; then
  chgrp -R gengin-llmopt "$CHECKOUT/llmOpt/gengin"
  chmod -R g+rwX "$CHECKOUT/llmOpt/gengin"
  find "$CHECKOUT/llmOpt/gengin" -type d -exec chmod g+s {} + 2>/dev/null || true
fi
# Control-plane source must NOT be agent-writable.
chown -R root:root "$CHECKOUT/llmOpt/supervisor.py" "$CHECKOUT/llmOpt/openrouter_keys.py" \
  "$CHECKOUT/llmOpt/systemd" "$CHECKOUT/llmOpt/scripts/setup-vm.sh" 2>/dev/null || true
chmod 0644 "$CHECKOUT/llmOpt/supervisor.py" "$CHECKOUT/llmOpt/openrouter_keys.py" 2>/dev/null || true

# Seed llmOpt/.env from the example if absent (operator edits afterwards).
if [[ ! -f "$CHECKOUT/llmOpt/.env" ]]; then
  cp "$CHECKOUT/llmOpt/.env.example" "$CHECKOUT/llmOpt/.env"
  sed -i 's/^AGENT_USER=.*/AGENT_USER=llmopt-agent/' "$CHECKOUT/llmOpt/.env"
  log "created $CHECKOUT/llmOpt/.env from .env.example (AGENT_USER=llmopt-agent)"
fi
sed -i "s|^MCP_VENV=.*|MCP_VENV=$VENV|" "$CHECKOUT/llmOpt/.env"
if ! grep -q '^AGENT_USER=llmopt-agent' "$CHECKOUT/llmOpt/.env"; then
  log "WARNING: $CHECKOUT/llmOpt/.env should set AGENT_USER=llmopt-agent for two-user mode"
fi
chown llmopt-supervisor:gengin-llmopt "$CHECKOUT/llmOpt/.env"
chmod 0640 "$CHECKOUT/llmOpt/.env"

# --- agent-launch helper (root-owned, not agent-writable) -------------------
log "installing agent-launch helper"
mkdir -p "$HELPER_DIR"
cat > "$HELPER" <<'HELPER_EOF'
#!/usr/bin/env bash
# Fixed agent-launch helper. Runs as llmopt-agent via a narrow sudoers rule.
# Validates every received path, then execs the project launcher in supervised
# mode. No shell is invoked and no arbitrary command can be selected.
#
# Usage: agent-launch <checkout> <hermes_home> <query_file> <usage_file> <model>
set -euo pipefail

checkout="${1:-}"
hermes_home="${2:-}"
query_file="${3:-}"
usage_file="${4:-}"
model="${5:-}"

[[ -n "$checkout" && -n "$hermes_home" && -n "$query_file" && -n "$usage_file" && -n "$model" ]] \
  || { echo "agent-launch: missing arguments" >&2; exit 2; }

[[ "$checkout" == /* && -d "$checkout" ]] \
  || { echo "agent-launch: invalid checkout" >&2; exit 2; }
run_root="$checkout/llmOpt/run"

under() { # under ROOT PATH -> 0 if the canonical PATH is strictly under ROOT
  local root="$1" path="$2" real
  real="$(readlink -m "$path")"
  [[ "$real" == "$root"/* ]] || return 1
  return 0
}

under "$run_root" "$hermes_home" || { echo "agent-launch: hermes_home outside $run_root" >&2; exit 2; }
under "$run_root" "$query_file"  || { echo "agent-launch: query_file outside $run_root" >&2; exit 2; }
under "$run_root" "$usage_file"  || { echo "agent-launch: usage_file outside $run_root" >&2; exit 2; }
[[ -d "$hermes_home" && -f "$query_file" ]] \
  || { echo "agent-launch: hermes_home or query_file missing" >&2; exit 2; }
# Model ids may carry an OpenRouter routing variant suffix (`:floor`, `:free`).
[[ "$model" =~ ^[A-Za-z0-9._:/-]+$ ]] || { echo "agent-launch: bad model" >&2; exit 2; }

# Secrets arrive through the preserved environment (sudoers env_keep), never
# via argv.
[[ -n "${OPENROUTER_API_KEY:-}" ]] || { echo "agent-launch: OPENROUTER_API_KEY not set" >&2; exit 2; }
[[ -n "${HERMES_HOME:-}" ]] || { echo "agent-launch: HERMES_HOME not set" >&2; exit 2; }

launcher="$checkout/llmOpt/scripts/gengin-opt.sh"
[[ -x "$launcher" ]] || { echo "agent-launch: launcher not executable: $launcher" >&2; exit 2; }

export PATH="$HOME/.local/bin:$PATH"
exec "$launcher" openrouter --supervised \
  --model "$model" --query-file "$query_file" --usage-file "$usage_file"
HELPER_EOF
chown root:root "$HELPER"
chmod 0755 "$HELPER"

# --- agent-kill helper (root-owned; signal relay for two-user mode) --------
# llmopt-supervisor cannot signal llmopt-agent's processes; termination goes
# through this fixed helper running as root via a narrow sudoers rule.
KILL_HELPER="$HELPER_DIR/agent-kill"
log "installing agent-kill helper"
cat > "$KILL_HELPER" <<'KILL_EOF'
#!/usr/bin/env bash
# Fixed process-group signal helper. Usage: agent-kill <TERM|KILL|probe> <pgid>
set -euo pipefail

action="${1:-}"
pgid="${2:-}"

[[ "$pgid" =~ ^[0-9]+$ ]] || { echo "agent-kill: bad pgid" >&2; exit 2; }
(( pgid > 1 )) || { echo "agent-kill: refusing pgid $pgid" >&2; exit 2; }

case "$action" in
  TERM)  sig=TERM ;;
  KILL)  sig=KILL ;;
  probe) sig=0 ;;
  *) echo "agent-kill: bad action" >&2; exit 2 ;;
esac

exec kill -s "$sig" -- "-$pgid"
KILL_EOF
chown root:root "$KILL_HELPER"
chmod 0755 "$KILL_HELPER"

# --- sudoers rule (exact, no shell, no reciprocal sudo) --------------------
log "installing sudoers rule"
cat > "$SUDOERS" <<SUDOERS_EOF
# gengin-llmopt: supervisor may run the fixed agent-launch helper as llmopt-agent.
# Only the environment variables the helper needs survive the UID switch
# (GENGIN_* are the expansion sources for the per-session Hermes config).
Defaults:llmopt-supervisor env_reset
Defaults:llmopt-supervisor env_keep += "OPENROUTER_API_KEY HERMES_HOME DISPLAY LIBGL_ALWAYS_SOFTWARE PYTHONUNBUFFERED GENGIN_TARGET_SHA GENGIN_REPO_URL GENGIN_TARGET_BRANCH GENGIN_SESSION_ID GENGIN_SESSION_RESULT_PATH GENGIN_INPUTS_DIR GITHUB_TOKEN HOME PATH"
llmopt-supervisor ALL=(llmopt-agent) NOPASSWD: $HELPER
# Signal relay: the supervisor cannot signal the agent UID; this fixed helper
# (root) may SIGTERM/SIGKILL a validated process group.
llmopt-supervisor ALL=(root) NOPASSWD: $KILL_HELPER
SUDOERS_EOF
chown root:root "$SUDOERS"
chmod 0440 "$SUDOERS"
visudo -cf "$SUDOERS" >/dev/null || die "sudoers file failed validation"

# --- inputs dir + manifest --------------------------------------------------
log "provisioning inputs dir: $INPUTS"
mkdir -p "$INPUTS"
for rel in deps assets .flamegraph; do
  if [[ -e "$CHECKOUT/$rel" ]]; then
    rsync -a "$CHECKOUT/$rel/" "$INPUTS/$rel/"
  fi
done
[[ -f "$CHECKOUT/default.profdata" ]] && cp -f "$CHECKOUT/default.profdata" "$INPUTS/default.profdata"
# Generate the manifest with the supervisor's own code (authoritative).
( cd "$CHECKOUT/llmOpt" && python3 -c "import main; print(main.generate_inputs_manifest('$INPUTS'))" ) \
  || die "inputs manifest generation failed"
chown -R root:root "$INPUTS"
chmod -R a+rX "$INPUTS"
# The agent reads inputs during a session; the supervisor verifies the manifest.
chmod 0755 "$INPUTS"

# --- MCP venv ---------------------------------------------------------------
log "creating venv: $VENV"
if [[ ! -x "$VENV/bin/python" ]]; then
  python3 -m venv "$VENV"
fi
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet -r "$CHECKOUT/llmOpt/requirements-mcp.txt"
chown -R llmopt-agent:llmopt-agent "$VENV"

# --- secrets: ONE root-only file (management key + optional GITHUB_TOKEN) ---
# Consumed as EnvironmentFile= by the systemd unit and sourced by the console
# launcher. It must stay root-only: llmOpt/ is group-readable by the agent,
# which must never see the management key. Seeded from legacy locations.
SECRETS=/etc/gengin-llmopt/secrets.env
# Dir is traversable (0711) so the supervisor can stat the file for the
# credential perms check; the file itself stays root-only 600 (content gate).
mkdir -p /etc/gengin-llmopt
chown root:root /etc/gengin-llmopt
chmod 0711 /etc/gengin-llmopt
if [[ ! -f "$SECRETS" ]]; then
  mgmt="REPLACE_ME_MANAGEMENT_KEY"
  [[ -f "$CRED_DIR/OPENROUTER_MANAGEMENT_KEY" ]] && mgmt="$(cat "$CRED_DIR/OPENROUTER_MANAGEMENT_KEY")"
  gh=""
  [[ -f /etc/gengin-llmopt/agent.env ]] && gh="$(sed -n 's/^GITHUB_TOKEN=//p' /etc/gengin-llmopt/agent.env | head -1)"
  printf 'OPENROUTER_MANAGEMENT_KEY=%s\nGITHUB_TOKEN=%s\n' "$mgmt" "$gh" > "$SECRETS"
  log "WARNING: seeded $SECRETS — review it before use:"
  log "  sudo nano $SECRETS"
fi
chown root:root "$SECRETS"
chmod 0600 "$SECRETS"

# --- Hermes agent (as the agent user) ---------------------------------------
if ! sudo -H -u llmopt-agent bash -lc 'command -v hermes >/dev/null' 2>/dev/null; then
  log "installing Hermes Agent as llmopt-agent"
  sudo -H -u llmopt-agent bash -c '
    curl -fsSL https://raw.githubusercontent.com/NousResearch/hermes-agent/main/scripts/install.sh | bash
  ' </dev/null 2>&1 | tail -5 || log "WARNING: Hermes install failed — install it manually as llmopt-agent"
fi
# Expose the agent's hermes on the system PATH so availability checks and the
# fixed launcher helper find it regardless of HOME. The agent home must be
# traversable (not listable) for other users to resolve the symlink.
AGENT_HERMES=/home/llmopt-agent/.local/bin/hermes
chmod 711 /home/llmopt-agent
if [[ -x "$AGENT_HERMES" ]]; then
  ln -sf "$AGENT_HERMES" /usr/local/bin/hermes
  log "linked $AGENT_HERMES -> /usr/local/bin/hermes"
else
  log "WARNING: agent hermes not found at $AGENT_HERMES"
fi

# --- systemd units ----------------------------------------------------------
log "installing systemd units"
for unit in gengin-xvfb gengin-openrouter-proxy gengin-llmopt; do
  src="$CHECKOUT/llmOpt/systemd/$unit.service"
  # A failed sed used to leave a zero-length unit behind, which systemd reads
  # as masked: the service silently exists but refuses to start.  Render to a
  # temp file and only then replace the installed unit.
  [[ -f "$src" ]] || die "missing unit template: $src"
  tmp="$(mktemp)"
  sed -e "s|__CHECKOUT__|$CHECKOUT|g" -e "s|__VENV__|$VENV|g" "$src" > "$tmp"
  install -o root -g root -m 0644 "$tmp" "/etc/systemd/system/$unit.service"
  rm -f "$tmp"
done
systemctl daemon-reload

# --- perf -------------------------------------------------------------------
if [[ "$SKIP_PERF" -eq 0 ]]; then
  log "enabling unprivileged perf counters"
  bash "$CHECKOUT/llmOpt/scripts/enable-perf.sh" || log "perf enable failed (continue; REQUIRE_PERF governs)"
fi

# --- bot git identity (agent) ----------------------------------------------
# sudo -H is required: otherwise HOME stays /root, so `git config --global`
# writes nowhere and the ~/.ssh expansion points into root's home.
log "configuring bot git identity for llmopt-agent"
sudo -H -u llmopt-agent git config --global user.name "gengin-llmopt" 2>/dev/null || true
sudo -H -u llmopt-agent git config --global user.email "gengin-llmopt@users.noreply.github.com" 2>/dev/null || true
# SSH host checking must stay on; never set StrictHostKeyChecking=no.
sudo -H -u llmopt-agent install -d -m 700 /home/llmopt-agent/.ssh

log "provisioning complete"
log "next steps:"
log "  1. review llmOpt/.env (seeded from .env.example)"
log "  2. review /etc/gengin-llmopt/secrets.env (management key + GITHUB_TOKEN)"
log "  3. copy the agent's SSH key into the agent's environment"
log "  4. sudo systemctl enable --now gengin-xvfb.service"
log "  5. sudo systemctl enable --now gengin-llmopt.service"
log "  6. journalctl -u gengin-llmopt.service -f"
