#!/usr/bin/env python3
"""gengin-llmopt supervisor.

Watches the newest commit on the configured remote branch, prepares an
exact-SHA sandbox, creates a budget-capped temporary OpenRouter key,
supervises a Hermes optimization session, and opens one pull request per
validated optimization.

Commands:
    run                     persistent polling loop (systemd)
    --preflight             validate configuration, tools, display, OpenCL,
                            checkout, build, and short benchmark
    --once                  poll and process at most one eligible SHA, then exit
    --dry-run               resolve configuration and report intent, no side effects
    --status                print a sanitized state summary
    --cleanup-stale-keys    reconcile supervisor-created OpenRouter keys

Exit codes: 0 ok, 1 runtime failure, 2 invalid configuration, 3 corrupt state.
"""

import errno
import fcntl
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation

LLMOPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(LLMOPT_DIR)
# Overridable for alternate deployments/tests; defaults to llmOpt/.env.
ENV_FILE = os.environ.get("GENGIN_LLMOPT_ENV", os.path.join(LLMOPT_DIR, ".env"))
STATE_SCHEMA_VERSION = 1
KEY_NAME_PREFIX = "gengin-llmopt-"
# Root-owned helper installed by setup-vm.sh; the only command the supervisor
# may run as the agent user (see the generated sudoers rule).
AGENT_LAUNCH_HELPER = "/usr/local/lib/gengin-llmopt/agent-launch"
AGENT_KILL_HELPER = "/usr/local/lib/gengin-llmopt/agent-kill"
AGENT_GROUP = "gengin-llmopt"

EXIT_OK = 0
EXIT_RUNTIME = 1
EXIT_CONFIG = 2
EXIT_STATE_CORRUPT = 3

# Checks whose failure may be caused by transient network/resource conditions;
# they do not count toward the deterministic quarantine threshold.
TRANSIENT_CHECKS = {"remote", "utc", "openrouter_model", "openrouter_credit"}

# (name, minimum, maximum) bounds for integer settings.
INT_BOUNDS = {
    "POLL_INTERVAL_SECONDS": (1, 86400),
    "SESSION_TIMEOUT_SECONDS": (60, 604800),
    "BUDGET_POLL_SECONDS": (1, 3600),
    "MAX_SETUP_RETRIES": (1, 100),
    "RETRY_BASE_SECONDS": (1, 3600),
    "RETRY_MAX_SECONDS": (1, 86400),
    "KEY_EXPIRY_GRACE_SECONDS": (1, 86400),
    "TERMINATION_GRACE_SECONDS": (1, 3600),
    "LOG_RETENTION_DAYS": (1, 3650),
    "PREFLIGHT_BENCH_DURATION_SECONDS": (1, 600),
}

KNOWN_KEYS = {
    "GENGIN_REPO_URL",
    "GENGIN_INPUTS_DIR",
    "WATCH_REMOTE",
    "WATCH_BRANCH",
    "POLL_INTERVAL_SECONDS",
    "RUN_ON_START",
    "MAX_SETUP_RETRIES",
    "RETRY_BASE_SECONDS",
    "RETRY_MAX_SECONDS",
    "OPENROUTER_MODEL",
    "OPENROUTER_BUDGET_USD",
    "SESSION_TIMEOUT_SECONDS",
    "BUDGET_POLL_SECONDS",
    "KEY_EXPIRY_GRACE_SECONDS",
    "TERMINATION_GRACE_SECONDS",
    "GENGIN_DISPLAY",
    "LIBGL_ALWAYS_SOFTWARE",
    "HEADLESS_MODE",
    "PREFLIGHT_BENCH_DURATION_SECONDS",
    "REQUIRE_PERF",
    "LOG_RETENTION_DAYS",
    "STATE_DIR",
    "SESSION_LOG_DIR",
    "RUN_DIR",
    "AGENT_USER",
    "MCP_VENV",
}

HEX_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class ConfigError(Exception):
    pass


class StateCorruptError(Exception):
    pass


class LockHeldError(Exception):
    def __init__(self, owner_pid):
        super().__init__(f"supervisor lock is held by pid {owner_pid}")
        self.owner_pid = owner_pid


_shutdown = threading.Event()


def _install_signal_handlers():
    """Orderly shutdown on SIGTERM/SIGINT: the active session is terminated and
    the temporary key deleted before exit."""
    def handle(signum, _frame):
        log("WARN", "supervisor.signal", signum=signum)
        _shutdown.set()

    signal.signal(signal.SIGTERM, handle)
    signal.signal(signal.SIGINT, handle)


def utc_now():
    return datetime.now(timezone.utc)


def log(level, event, session_id=None, **fields):
    ts = utc_now().strftime("%Y-%m-%dT%H:%M:%SZ")
    parts = [ts, level, event]
    if session_id:
        parts.append(f"session={session_id}")
    for key, value in fields.items():
        parts.append(f"{key}={value}")
    print(" ".join(parts), file=sys.stderr, flush=True)


def redact(text, secrets):
    for secret in secrets:
        if secret:
            text = text.replace(secret, "[REDACTED]")
    return text


def parse_bool(value, key, errors):
    lowered = value.strip().lower()
    if lowered in ("true", "1", "yes"):
        return True
    if lowered in ("false", "0", "no"):
        return False
    errors.append(f"{key}: expected true/false, got {value!r}")
    return None


def parse_int(value, key, errors):
    try:
        number = int(value)
    except ValueError:
        errors.append(f"{key}: expected integer, got {value!r}")
        return None
    low, high = INT_BOUNDS[key]
    if not low <= number <= high:
        errors.append(f"{key}: {number} outside allowed range {low}..{high}")
        return None
    return number


def parse_env_file(path):
    """Strict KEY=VALUE parser.

    Blank lines and '#' comments are ignored. Values may be wrapped in
    matching single or double quotes. Duplicate and unknown keys are
    rejected. The file is never shell-sourced.
    """
    values = {}
    errors = []
    try:
        with open(path, encoding="utf-8") as handle:
            lines = handle.readlines()
    except OSError as exc:
        return values, [f"cannot read {path}: {exc}"]
    for lineno, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            errors.append(f"line {lineno}: expected KEY=VALUE")
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        if not KEY_RE.fullmatch(key):
            errors.append(f"line {lineno}: invalid key {key!r}")
            continue
        if key not in KNOWN_KEYS:
            errors.append(f"line {lineno}: unknown key {key!r}")
            continue
        if key in values:
            errors.append(f"line {lineno}: duplicate key {key!r}")
            continue
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]
        values[key] = value
    return values, errors


@dataclass
class Config:
    gengin_repo_url: str
    gengin_inputs_dir: str
    watch_remote: str
    watch_branch: str
    poll_interval_seconds: int
    run_on_start: bool
    max_setup_retries: int
    retry_base_seconds: int
    retry_max_seconds: int
    openrouter_model: str
    openrouter_budget_usd: Decimal
    session_timeout_seconds: int
    budget_poll_seconds: int
    key_expiry_grace_seconds: int
    termination_grace_seconds: int
    gengin_display: str
    libgl_always_software: str
    headless_mode: str
    preflight_bench_duration_seconds: int
    require_perf: bool
    log_retention_days: int
    state_dir: str
    session_log_dir: str
    run_dir: str
    agent_user: str = ""
    mcp_venv: str = ""
    management_key: str = ""

    def mcp_python(self):
        """Interpreter for the MCP server (needs the `mcp` package)."""
        if self.mcp_venv:
            return os.path.join(self.mcp_venv, "bin", "python")
        return sys.executable

    def resolved(self, path):
        if os.path.isabs(path):
            return os.path.realpath(path)
        return os.path.realpath(os.path.join(REPO_ROOT, path))


def _require(values, key, errors):
    value = values.get(key)
    if value is None or value == "":
        errors.append(f"{key}: missing or empty")
        return None
    return value


def _management_key_from_env_or_credential():
    """Management key from the environment, else a credential file.

    Sources, in order: $OPENROUTER_MANAGEMENT_KEY, then
    $CREDENTIALS_DIRECTORY/OPENROUTER_MANAGEMENT_KEY (systemd LoadCredential
    exposes the file there without setting an environment variable). The file
    may be a raw key or dotenv-style KEY=VALUE lines (single secrets file).
    """
    key = os.environ.get("OPENROUTER_MANAGEMENT_KEY", "")
    if key:
        return key
    cred_dir = os.environ.get("CREDENTIALS_DIRECTORY", "")
    if cred_dir:
        path = os.path.join(cred_dir, "OPENROUTER_MANAGEMENT_KEY")
        try:
            with open(path) as fh:
                content = fh.read().strip()
        except OSError:
            return ""
        for line in content.splitlines():
            line = line.strip()
            if line.startswith("OPENROUTER_MANAGEMENT_KEY="):
                return line.split("=", 1)[1].strip().strip("'\"")
        return content
    return ""


def load_config(require_management_key=True):
    """Parse and validate llmOpt/.env. Returns (config, errors)."""
    values, errors = parse_env_file(ENV_FILE)
    if errors:
        return None, errors

    def text(key):
        return _require(values, key, errors)

    repo_url = text("GENGIN_REPO_URL")
    inputs_dir = text("GENGIN_INPUTS_DIR")
    watch_remote = text("WATCH_REMOTE")
    watch_branch = text("WATCH_BRANCH")
    model = text("OPENROUTER_MODEL")
    display = text("GENGIN_DISPLAY")
    headless_mode = text("HEADLESS_MODE")
    state_dir = text("STATE_DIR")
    session_log_dir = text("SESSION_LOG_DIR")
    run_dir = text("RUN_DIR")

    budget_raw = _require(values, "OPENROUTER_BUDGET_USD", errors)
    budget = None
    if budget_raw is not None:
        try:
            budget = Decimal(budget_raw)
            if not budget.is_finite() or budget <= 0:
                errors.append("OPENROUTER_BUDGET_USD: must be a finite decimal > 0")
                budget = None
            elif budget > Decimal("10000"):
                errors.append("OPENROUTER_BUDGET_USD: implausibly large (> 10000)")
                budget = None
        except InvalidOperation:
            errors.append(f"OPENROUTER_BUDGET_USD: not a decimal: {budget_raw!r}")

    ints = {}
    for key in INT_BOUNDS:
        raw = values.get(key)
        if raw is None:
            errors.append(f"{key}: missing")
            continue
        ints[key] = parse_int(raw, key, errors)

    run_on_start = parse_bool(values.get("RUN_ON_START", ""), "RUN_ON_START", errors)
    require_perf = parse_bool(values.get("REQUIRE_PERF", ""), "REQUIRE_PERF", errors)
    libgl = values.get("LIBGL_ALWAYS_SOFTWARE", "1")
    agent_user = values.get("AGENT_USER", "").strip()
    if agent_user and not re.fullmatch(r"[a-z_][a-z0-9_-]*", agent_user):
        errors.append(f"AGENT_USER: invalid user name {agent_user!r}")
    mcp_venv = values.get("MCP_VENV", "").strip()
    if mcp_venv and not os.path.isabs(mcp_venv):
        errors.append("MCP_VENV: must be an absolute path")

    if model is not None and "/" not in model:
        errors.append("OPENROUTER_MODEL: expected provider/model format")
    if headless_mode is not None and headless_mode != "xvfb":
        errors.append(f"HEADLESS_MODE: only 'xvfb' is implemented, got {headless_mode!r}")
    if inputs_dir is not None and not os.path.isabs(inputs_dir):
        errors.append("GENGIN_INPUTS_DIR: must be an absolute path")
    if ints.get("BUDGET_POLL_SECONDS") is not None and ints.get("SESSION_TIMEOUT_SECONDS") is not None:
        if ints["BUDGET_POLL_SECONDS"] >= ints["SESSION_TIMEOUT_SECONDS"]:
            errors.append("BUDGET_POLL_SECONDS must be < SESSION_TIMEOUT_SECONDS")
    if ints.get("KEY_EXPIRY_GRACE_SECONDS") is not None and ints.get("TERMINATION_GRACE_SECONDS") is not None:
        if ints["KEY_EXPIRY_GRACE_SECONDS"] <= ints["TERMINATION_GRACE_SECONDS"]:
            errors.append("KEY_EXPIRY_GRACE_SECONDS must be > TERMINATION_GRACE_SECONDS")
    if ints.get("RETRY_BASE_SECONDS") is not None and ints.get("RETRY_MAX_SECONDS") is not None:
        if ints["RETRY_MAX_SECONDS"] < ints["RETRY_BASE_SECONDS"]:
            errors.append("RETRY_MAX_SECONDS must be >= RETRY_BASE_SECONDS")

    management_key = _management_key_from_env_or_credential()
    if require_management_key and not management_key:
        errors.append(
            "OPENROUTER_MANAGEMENT_KEY: not found (set the environment variable "
            "or provide the secrets/credential file)")

    if errors:
        return None, errors

    config = Config(
        gengin_repo_url=repo_url,
        gengin_inputs_dir=inputs_dir,
        watch_remote=watch_remote,
        watch_branch=watch_branch,
        poll_interval_seconds=ints["POLL_INTERVAL_SECONDS"],
        run_on_start=run_on_start,
        max_setup_retries=ints["MAX_SETUP_RETRIES"],
        retry_base_seconds=ints["RETRY_BASE_SECONDS"],
        retry_max_seconds=ints["RETRY_MAX_SECONDS"],
        openrouter_model=model,
        openrouter_budget_usd=budget,
        session_timeout_seconds=ints["SESSION_TIMEOUT_SECONDS"],
        budget_poll_seconds=ints["BUDGET_POLL_SECONDS"],
        key_expiry_grace_seconds=ints["KEY_EXPIRY_GRACE_SECONDS"],
        termination_grace_seconds=ints["TERMINATION_GRACE_SECONDS"],
        gengin_display=display,
        libgl_always_software=libgl,
        headless_mode=headless_mode,
        preflight_bench_duration_seconds=ints["PREFLIGHT_BENCH_DURATION_SECONDS"],
        require_perf=require_perf,
        log_retention_days=ints["LOG_RETENTION_DAYS"],
        state_dir=config_resolve(state_dir, "STATE_DIR", errors),
        session_log_dir=config_resolve(session_log_dir, "SESSION_LOG_DIR", errors),
        run_dir=config_resolve(run_dir, "RUN_DIR", errors),
        agent_user=agent_user,
        mcp_venv=mcp_venv,
        management_key=management_key,
    )
    if errors:
        return None, errors
    return config, []


def config_resolve(path, key, errors):
    resolved = os.path.realpath(os.path.join(REPO_ROOT, path))
    if os.path.commonpath([REPO_ROOT, resolved]) != REPO_ROOT:
        errors.append(f"{key}: must resolve beneath the repository root")
        return path
    return resolved


def default_state():
    return {
        "schemaVersion": STATE_SCHEMA_VERSION,
        "lastObservedSha": None,
        "lastProcessedSha": None,
        "pendingSha": None,
        "setupFailures": {},
        "activeSession": None,
        "keysPendingDeletion": [],
    }


def load_state(state_dir):
    path = os.path.join(state_dir, "supervisor.json")
    if not os.path.exists(path):
        return default_state()
    try:
        with open(path, encoding="utf-8") as handle:
            state = json.load(handle)
    except (json.JSONDecodeError, OSError) as exc:
        corrupt = f"{path}.corrupt-{utc_now().strftime('%Y%m%dT%H%M%SZ')}"
        try:
            os.replace(path, corrupt)
        except OSError:
            pass
        raise StateCorruptError(f"state file {path} is unreadable ({exc}); moved to {corrupt}")
    if not isinstance(state, dict) or state.get("schemaVersion") != STATE_SCHEMA_VERSION:
        corrupt = f"{path}.corrupt-{utc_now().strftime('%Y%m%dT%H%M%SZ')}"
        try:
            os.replace(path, corrupt)
        except OSError:
            pass
        raise StateCorruptError(f"state file {path} has unsupported schema; moved to {corrupt}")
    merged = default_state()
    merged.update(state)
    return merged


def save_state(state_dir, state):
    os.makedirs(state_dir, exist_ok=True)
    path = os.path.join(state_dir, "supervisor.json")
    tmp = f"{path}.tmp.{os.getpid()}"
    payload = json.dumps(state, indent=2, sort_keys=True) + "\n"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    dir_fd = os.open(state_dir, os.O_RDONLY)
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)


class SupervisorLock:
    def __init__(self, state_dir):
        self.state_dir = state_dir
        self.path = os.path.join(state_dir, "supervisor.lock")
        self.fd = None

    def acquire(self):
        os.makedirs(self.state_dir, exist_ok=True)
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            owner = self._read_owner()
            os.close(self.fd)
            self.fd = None
            raise LockHeldError(owner)
        os.ftruncate(self.fd, 0)
        os.write(self.fd, f"{os.getpid()}\n".encode())
        os.fsync(self.fd)

    def _read_owner(self):
        try:
            with open(self.path, encoding="utf-8") as handle:
                content = handle.read().strip()
            if content.isdigit():
                return int(content)
        except OSError:
            pass
        return -1

    def release(self):
        if self.fd is None:
            return
        try:
            fcntl.flock(self.fd, fcntl.LOCK_UN)
            os.close(self.fd)
        except OSError:
            pass
        self.fd = None


def validate_sha(value):
    return bool(HEX_SHA_RE.fullmatch(value or ""))


def remote_main_sha(repo_url, branch):
    """Query exactly one remote ref. Returns (sha, error)."""
    import subprocess

    try:
        result = subprocess.run(
            ["git", "ls-remote", "--exit-code", repo_url, f"refs/heads/{branch}"],
            capture_output=True,
            text=True,
            timeout=120,
        )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return None, str(exc)
    if result.returncode != 0:
        return None, result.stderr.strip() or f"git ls-remote exited {result.returncode}"
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        return None, f"expected exactly one ref result, got {len(lines)}"
    sha = lines[0].split()[0]
    if not validate_sha(sha):
        return None, f"invalid object id {sha!r}"
    return sha, None


def poll_once(config, state):
    """Query the remote tip and update observed/pending SHA.

    Returns (tip_sha, action) where action is one of:
      "first_start"   new state, recorded without launching (RUN_ON_START=false)
      "run_on_start"  new state, RUN_ON_START=true, tip is pending
      "pending"       tip differs from lastProcessedSha, tip is pending
      "unchanged"     tip equals lastProcessedSha, nothing to do
      "coalesced"     a newer tip replaced an older pending SHA
    """
    tip, err = remote_main_sha(config.gengin_repo_url, config.watch_branch)
    if err:
        log("WARN", "poll.remote_failed", detail=err)
        return None, "remote_failed"

    if state["lastObservedSha"] is None:
        state["lastObservedSha"] = tip
        if config.run_on_start:
            state["pendingSha"] = tip
            log("INFO", "commit.detected", sha=tip, first_start=True, run_on_start=True)
            return tip, "run_on_start"
        state["lastProcessedSha"] = tip
        log("INFO", "poll.first_start", sha=tip, run_on_start=False)
        return tip, "first_start"

    if tip == state["lastProcessedSha"]:
        state["lastObservedSha"] = tip
        return tip, "unchanged"

    state["lastObservedSha"] = tip
    if state["pendingSha"] is not None and state["pendingSha"] != tip:
        log("INFO", "commit.coalesced", dropped=state["pendingSha"], kept=tip)
    state["pendingSha"] = tip
    log("INFO", "commit.detected", sha=tip)
    return tip, "pending"


def prepare_sandbox(config, target_sha, session_id):
    """Prepare the exact-SHA sandbox via main.py. Returns main.py's result dict."""
    import main as gengin_main

    inputs_dir = config.gengin_inputs_dir if os.path.isdir(config.gengin_inputs_dir) else None
    log("INFO", "sandbox.prepare.started", sha=target_sha, session=session_id,
        inputs="manifest" if inputs_dir else "legacy-parent")
    result = gengin_main.git_pull_project(
        config.gengin_repo_url, config.watch_branch, target_sha,
        inputs_dir=inputs_dir, session_id=session_id,
        agent_group=AGENT_GROUP if config.agent_user else None,
    )
    log("INFO", "sandbox.prepare.ok", sha=target_sha,
        descendant_of_branch=result.get("descendantOfBranch"))
    return result


def _hermes_bin(config):
    """Path to the hermes CLI: the agent's own install in two-user mode."""
    if config.agent_user:
        cand = os.path.join(_agent_home(config), ".local", "bin", "hermes")
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return shutil.which("hermes")


def check_tools(config):
    """Deterministic tool availability checks. Returns list of (name, ok, detail)."""
    results = []

    def which(name):
        path = shutil.which(name)
        results.append((name, path is not None, path or "not found"))

    for name in ("git", "make", "clang", "python3", "clangd", "rsync",
                 "Xvfb", "xdpyinfo", "glxinfo", "clinfo", "perl"):
        which(name)
    hermes = _hermes_bin(config)
    results.append(("hermes", hermes is not None, hermes or "not found"))
    results.append(("/usr/bin/ld", os.path.exists("/usr/bin/ld"),
                    "/usr/bin/ld" if os.path.exists("/usr/bin/ld") else "not found"))
    if config.require_perf:
        which("perf")
    which("dot")
    which("gprof2dot")
    return results


def _run_cmd(cmd, timeout=120, cwd=None, env=None):
    import subprocess

    try:
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=timeout, cwd=cwd, env=env)
        return result.returncode, result.stdout, result.stderr
    except (subprocess.TimeoutExpired, OSError) as exc:
        return 124, "", str(exc)


def check_display(config):
    rc, out, err = _run_cmd(["xdpyinfo"], env={**os.environ, "DISPLAY": config.gengin_display})
    if rc == 0:
        return ("display", True, f"DISPLAY={config.gengin_display} reachable")
    return ("display", False, f"xdpyinfo failed: {err.strip()[:200]}")


def check_gl(config):
    rc, out, err = _run_cmd(["glxinfo", "-B"], env={**os.environ, "DISPLAY": config.gengin_display,
                                                     "LIBGL_ALWAYS_SOFTWARE": config.libgl_always_software})
    renderer = next((l.split(":", 1)[1].strip() for l in out.splitlines()
                     if "OpenGL renderer" in l), None)
    if rc == 0 and renderer:
        return ("gl", True, f"renderer={renderer}")
    return ("gl", False, f"glxinfo failed: {(err or out).strip()[:200]}")


def check_opencl():
    rc, out, err = _run_cmd(["clinfo"])
    if rc == 0 and "Platform" in out:
        platforms = sum(1 for l in out.splitlines() if l.strip().startswith("Platform"))
        return ("opencl", True, f"{platforms} platform(s)")
    return ("opencl", False, f"clinfo found no platform: {(err or out).strip()[:200]}")


def check_minifb(sandbox):
    header = os.path.join(sandbox, "deps", "minifb", "include", "MiniFB.h")
    lib = os.path.join(sandbox, "deps", "minifb", "build", "libminifb.a")
    if os.path.isfile(header) and os.path.isfile(lib):
        return ("minifb", True, "header + static lib present")
    missing = [p for p in (header, lib) if not os.path.isfile(p)]
    return ("minifb", False, f"missing: {missing}")


def check_assets(sandbox):
    required = ["assets/models/f16.mtl", "simulation/simModels/F-16C.bin"]
    missing = [rel for rel in required if not os.path.exists(os.path.join(sandbox, rel))]
    if missing:
        return ("assets", False, f"missing: {missing}")
    return ("assets", True, "required assets present")


def check_build(sandbox):
    rc, out, err = _run_cmd(["make"], timeout=1800, cwd=sandbox)
    if rc == 0:
        return ("build", True, "make succeeded")
    return ("build", False, f"make failed: {(err or out).strip()[-300:]}")


def check_short_bench(config, sandbox):
    env = {**os.environ, "DISPLAY": config.gengin_display,
           "LIBGL_ALWAYS_SOFTWARE": config.libgl_always_software}
    rc, out, err = _run_cmd(
        ["make", "bench", f"BENCH_DURATION={config.preflight_bench_duration_seconds}"],
        timeout=1800, cwd=sandbox, env=env)
    results = os.path.join(sandbox, "bench", "results", "bench_results.json")
    if rc == 0 and os.path.isfile(results):
        import json as _json
        try:
            with open(results) as f:
                data = _json.load(f)
            if data.get("frames", 0) > 0:
                return ("short_bench", True, f"frames={data['frames']}")
        except (OSError, _json.JSONDecodeError):
            pass
    return ("short_bench", False, f"bench failed or no valid results: {(err or out).strip()[-300:]}")


def check_perf(config):
    if not config.require_perf:
        return ("perf", True, "not required")
    rc, out, err = _run_cmd(["perf", "stat", "-e", "cycles", "true"], timeout=30)
    if rc == 0:
        return ("perf", True, "unprivileged counters work")
    return ("perf", False, f"perf blocked: {err.strip()[:200]} (run enable-perf.sh)")


def check_disk(config):
    import shutil

    # Allow ~10 GB headroom for a sandbox backup, build outputs, profiles, logs.
    need = 10 * 1024 ** 3
    usage = shutil.disk_usage(REPO_ROOT)
    if usage.free >= need:
        return ("disk", True, f"free={usage.free // (1024**3)}GiB")
    return ("disk", False, f"free={usage.free // (1024**3)}GiB < {need // (1024**3)}GiB required")


def check_utc():
    """Clock sanity for key-expiration timestamps.

    Prefers systemd's NTP status; falls back to comparing the system clock
    against the OpenRouter HTTP Date header (the service that actually
    validates the expiry).
    """
    rc, out, _err = _run_cmd(["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
                             timeout=15)
    if rc == 0 and out.strip() == "yes":
        return ("utc", True, "systemd: NTP synchronized")

    import urllib.request
    import email.utils

    try:
        req = urllib.request.Request("https://openrouter.ai/api/v1/models",
                                     method="HEAD",
                                     headers={"User-Agent": "gengin-llmopt-supervisor/1.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            date_hdr = resp.headers.get("Date")
        if not date_hdr:
            return ("utc", False, "no Date header from OpenRouter")
        remote_dt = email.utils.parsedate_to_datetime(date_hdr)
        if remote_dt.tzinfo is None:
            remote_dt = remote_dt.replace(tzinfo=timezone.utc)
        drift = abs((utc_now() - remote_dt).total_seconds())
        if drift < 30:
            return ("utc", True, f"drift vs OpenRouter={drift:.1f}s")
        return ("utc", False, f"clock drift {drift:.1f}s >= 30s")
    except Exception as exc:
        return ("utc", False, f"could not verify UTC: {exc}")


def produce_baseline(sandbox):
    """Produce the full clean baseline (5-run makeBench + flight suite).

    Called after preflight succeeds and before key creation. Requires a clean
    sandbox at the target SHA.
    """
    import main as gengin_main

    old = gengin_main.PROJECT_DIR
    gengin_main.PROJECT_DIR = sandbox
    gengin_main.BASELINE_RESULTS = None
    try:
        result = gengin_main.makeBench()
        try:
            flight = gengin_main.flightBench(capture_baseline=True)
            if flight.get("capturedBaseline"):
                log("INFO", "flight_baseline.captured", suite=flight["suiteHash"],
                    miss=round(flight["aggregate"]["miss"], 1),
                    hitRate=round(flight["aggregate"]["hitRate"], 3),
                    costUs=round(flight["aggregate"]["costUs"], 1))
            else:
                log("WARN", "flight_baseline.not_captured",
                    detail=flight.get("summary", "")[:200].replace("\n", " | "))
        except (RuntimeError, OSError) as exc:
            log("WARN", "flight_baseline.failed", detail=str(exc)[:200])
        return result
    finally:
        gengin_main.PROJECT_DIR = old


def check_remote(config):
    """Remote branch is readable without an interactive prompt."""
    sha, err = remote_main_sha(config.gengin_repo_url, config.watch_branch)
    if err:
        return ("remote", False, f"cannot read {config.watch_branch}: {err[:160]}")
    return ("remote", True, f"{config.watch_branch}@{sha[:12]}")


def check_openrouter_model(config):
    """Configured model exists in the public catalog (no spending).

    Routing variant suffixes (`:floor`, `:free`, `:nitro`, ...) are accepted on
    top of any catalog model, and provider pins (`:xiaomi`, `:xiaomi/fp8`) are
    matched against the base model's endpoint list.  Pins are a harness
    convention: the proxy turns them into `provider.order`, because OpenRouter
    ignores unknown slug suffixes.
    """
    import openrouter_keys as ork

    model = config.openrouter_model
    available = ork.model_available(model)
    if available is None:
        return ("openrouter_model", False, "could not fetch the model catalog")
    if available:
        bare, _, rest = model.partition(":")
        suffixes = [s for s in rest.split(":") if s]
        variant = next((s for s in suffixes if s in ork.VARIANT_SUFFIXES), "")
        pins = [s for s in suffixes if s not in ork.VARIANT_SUFFIXES]
        detail = bare
        if variant:
            detail += f" (variant :{variant})"
        if pins:
            detail += f" (pin :{',:'.join(pins)} -> provider.order)"
        return ("openrouter_model", True, detail)
    return ("openrouter_model", False, f"model not found: {model}")


def check_openrouter_credit(config):
    """Account credit must cover the per-session limit (no key is created on failure)."""
    import openrouter_keys as ork

    try:
        client = ork.OpenRouterClient(config.management_key)
        data = client.get_credits()
    except ork.OpenRouterAuthError as exc:
        return ("openrouter_credit", False, f"management credential rejected: {exc}")
    except ork.OpenRouterError as exc:
        return ("openrouter_credit", False, f"could not verify account credit: {exc}")
    total = data.get("total_credits")
    usage = data.get("total_usage")
    if total is None:
        return ("openrouter_credit", False, "credit response missing total_credits")
    remaining = float(total) - float(usage or 0)
    if remaining >= float(config.openrouter_budget_usd):
        return ("openrouter_credit", True, f"remaining={remaining:.2f} USD")
    return ("openrouter_credit", False,
            f"remaining {remaining:.2f} USD < budget {config.openrouter_budget_usd} USD")


def check_credential_perms(config):
    """Credential files (when present) must not be readable beyond root.

    The single secrets file feeds the supervisor directly; the legacy systemd
    credential location may still exist on older installs.
    """
    paths = (
        "/etc/gengin-llmopt/secrets.env",
        "/etc/systemd/credentials/gengin-llmopt/OPENROUTER_MANAGEMENT_KEY",
    )
    present = [p for p in paths if os.path.exists(p)]
    if not present:
        return ("credential_perms", True, "no credential file (env-provided)")
    for path in present:
        mode = os.stat(path).st_mode
        if mode & 0o077:
            return ("credential_perms", False, f"{path} is readable beyond its owner")
    return ("credential_perms", True,
            "; ".join(f"{p} mode {oct(os.stat(p).st_mode & 0o777)}" for p in present))


def check_mcp_python(config):
    """The MCP interpreter must exist and import the mcp package."""
    python = config.mcp_python()
    if not os.path.isfile(python):
        return ("mcp_python", False, f"interpreter not found: {python}")
    rc, _out, err = _run_cmd([python, "-c", "import mcp"], timeout=60)
    if rc == 0:
        return ("mcp_python", True, python)
    return ("mcp_python", False, f"{python} cannot import mcp: {err.strip()[:200]}")


def check_agent_launch(config):
    """Verify the two-user launch path (helper installed, sudo works, home resolvable)."""
    if not config.agent_user:
        return ("agent_launch", True, "single-user mode (no UID switch)")
    try:
        import pwd
        home = pwd.getpwnam(config.agent_user).pw_dir
    except (KeyError, ImportError, OSError):
        return ("agent_launch", False, f"user {config.agent_user!r} does not exist")
    if not os.path.isfile(AGENT_LAUNCH_HELPER):
        return ("agent_launch", False, f"helper not installed: {AGENT_LAUNCH_HELPER}")
    if not os.access(AGENT_LAUNCH_HELPER, os.X_OK):
        return ("agent_launch", False, f"helper not executable: {AGENT_LAUNCH_HELPER}")
    # Invoking with no arguments exercises sudo + exec; the helper exits 2.
    rc, _out, err = _run_cmd(
        ["sudo", "-n", "-u", config.agent_user, AGENT_LAUNCH_HELPER], timeout=30)
    if rc in (0, 2):
        return ("agent_launch", True, f"sudo -> {config.agent_user} ok (home={home})")
    return ("agent_launch", False, f"helper invocation failed rc={rc}: {err.strip()[:200]}")


def check_agent_kill(config):
    """Verify the root kill helper works (two-user termination path)."""
    if not config.agent_user:
        return ("agent_kill", True, "single-user mode (direct signals)")
    if not os.path.isfile(AGENT_KILL_HELPER):
        return ("agent_kill", False, f"helper not installed: {AGENT_KILL_HELPER}")
    if not os.access(AGENT_KILL_HELPER, os.X_OK):
        return ("agent_kill", False, f"helper not executable: {AGENT_KILL_HELPER}")
    # Probing the supervisor's own process group exercises sudo + kill as root.
    rc = _helper_signal("probe", os.getpgrp())
    if rc == 0:
        return ("agent_kill", True, "sudo -> root helper ok")
    return ("agent_kill", False, f"helper probe failed rc={rc}")


def check_github_token():
    """WARN-level: sessions can still run without a valid token (no_change
    paths), but create_pr will fail — surface it before a session burns budget."""
    token = os.environ.get("GITHUB_TOKEN", "")
    if not token:
        return ("github_token", True, "WARN: GITHUB_TOKEN not set; create_pr will fail")
    import urllib.request
    import urllib.error
    req = urllib.request.Request(
        "https://api.github.com/user",
        headers={"Authorization": f"Bearer {token}",
                 "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            login = json.loads(resp.read()).get("login", "?")
        return ("github_token", True, f"valid (user {login})")
    except urllib.error.HTTPError as e:
        return ("github_token", True,
                f"WARN: token rejected HTTP {e.code}; create_pr will fail")
    except Exception as e:
        return ("github_token", True, f"WARN: token check failed: {e}")


def run_preflight(config, sandbox=None):
    """Run all deterministic preflight checks. Returns (exit_code, results).

    `sandbox` (the prepared exact-SHA dir) enables build/bench/asset checks;
    without it only infrastructure checks run.
    """
    log("INFO", "preflight.started")
    results = []
    results.extend(check_tools(config))
    results.append(check_remote(config))
    results.append(check_display(config))
    results.append(check_gl(config))
    results.append(check_opencl())
    results.append(check_utc())
    results.append(check_disk(config))
    results.append(check_openrouter_model(config))
    results.append(check_openrouter_credit(config))
    results.append(check_credential_perms(config))
    results.append(check_github_token())
    if sandbox:
        results.append(check_minifb(sandbox))
        results.append(check_assets(sandbox))
        results.append(check_build(sandbox))
        results.append(check_short_bench(config, sandbox))
    results.append(check_perf(config))
    results.append(check_agent_launch(config))
    results.append(check_agent_kill(config))
    results.append(check_mcp_python(config))

    failures = 0
    for name, ok, detail in results:
        log("INFO" if ok else "ERROR", "preflight.check", name=name,
            status="ok" if ok else "FAIL", detail=detail)
        if not ok:
            failures += 1
    if failures:
        log("ERROR", "preflight.failed", failures=failures)
        return EXIT_RUNTIME, results
    log("INFO", "preflight.ok")
    return EXIT_OK, results


def print_status(config, state):
    print("gengin-llmopt supervisor status")
    print(f"  state file:        {os.path.join(config.state_dir, 'supervisor.json')}")
    print(f"  last observed sha: {state['lastObservedSha'] or '(none)'}")
    print(f"  last processed sha:{' '}{state['lastProcessedSha'] or '(none)'}")
    print(f"  pending sha:       {state['pendingSha'] or '(none)'}")
    active = state["activeSession"]
    if active:
        print("  active session:")
        print(f"    id:          {active.get('sessionId')}")
        print(f"    target sha:  {active.get('targetSha')}")
        print(f"    phase:       {active.get('phase')}")
        print(f"    pid:         {active.get('pid')}")
        print(f"    deadline:    {active.get('deadlineAt')}")
        print(f"    log:         {active.get('logPath')}")
        key_hash = active.get("keyHash") or ""
        print(f"    key hash:    {key_hash[:12] + '...' if key_hash else '(none)'}")
    else:
        print("  active session:    (none)")
    print(f"  keys pending del: {len(state['keysPendingDeletion'])}")
    print(f"  setup failures:    {len(state['setupFailures'])}")


def print_dry_run(config, state):
    print("gengin-llmopt supervisor dry run (no side effects)")
    print(f"  repo url:      {config.gengin_repo_url}")
    print(f"  watch ref:     refs/heads/{config.watch_branch}")
    print(f"  poll interval: {config.poll_interval_seconds}s")
    print(f"  run on start:  {config.run_on_start}")
    print(f"  model:         {config.openrouter_model}")
    print(f"  budget:        {config.openrouter_budget_usd} USD")
    print(f"  session limit: {config.session_timeout_seconds}s")
    print(f"  display:       {config.gengin_display} (headless={config.headless_mode})")
    print(f"  inputs dir:    {config.gengin_inputs_dir}")
    print(f"  state dir:     {config.state_dir}")
    print(f"  log dir:       {config.session_log_dir}")
    print(f"  run dir:       {config.run_dir}")
    print(f"  last observed: {state['lastObservedSha'] or '(none)'}")
    print(f"  last processed:{' '}{state['lastProcessedSha'] or '(none)'}")
    print(f"  pending:       {state['pendingSha'] or '(none)'}")
    if state["activeSession"]:
        print("  NOTE: an active session is recorded; a new session will not start until it is reconciled.")
    print("  action: would poll the remote ref and, on a new SHA, prepare the exact-SHA")
    print("          sandbox, run preflight, create a capped temporary key, and launch Hermes.")


def run_once(config, state):
    """Poll once and process at most one eligible SHA (full pipeline).

    Persists state after each transition; stops before key creation if
    preparation or preflight fails.
    """
    tip, action = poll_once(config, state)
    if action == "remote_failed":
        return EXIT_RUNTIME
    if action in ("first_start", "unchanged"):
        save_state(config.state_dir, state)
        return EXIT_OK

    # action is "pending" or "run_on_start": process the pending SHA.
    target = state["pendingSha"] or tip
    session_id = f"{utc_now().strftime('%Y%m%dT%H%M%SZ')}-{target[:8]}"
    try:
        prepare_sandbox(config, target, session_id)
    except Exception as exc:
        if isinstance(exc, OSError) and exc.errno == errno.EBUSY:
            # A process cwd (interactive shell, editor) can hold the sandbox
            # directory; such holders clear on their own. Retry on the next
            # poll instead of counting toward quarantine.
            log("WARN", "sandbox.prepare.transient", sha=target, detail=str(exc))
            save_state(config.state_dir, state)
            return EXIT_RUNTIME
        failures = state["setupFailures"].get(target, 0) + 1
        state["setupFailures"][target] = failures
        log("ERROR", "sandbox.prepare.failed", sha=target, failures=failures, detail=str(exc))
        save_state(config.state_dir, state)
        if failures >= config.max_setup_retries:
            log("ERROR", "sandbox.quarantined", sha=target, failures=failures)
            state["setupFailures"].pop(target, None)
            state["pendingSha"] = None
            state["lastProcessedSha"] = target
            save_state(config.state_dir, state)
        return EXIT_RUNTIME

    # Sandbox is ready. Run preflight (build + short bench + infra) and produce
    # the full clean baseline BEFORE any model spending.
    sandbox = os.path.realpath(os.path.join(REPO_ROOT, "llmOpt", "gengin"))
    code, results = run_preflight(config, sandbox=sandbox)
    if code != EXIT_OK:
        failed = {name for name, ok, _detail in results if not ok}
        deterministic = failed - TRANSIENT_CHECKS
        if not deterministic:
            # Transient (network/resource) failures retry without counting
            # toward the deterministic quarantine threshold.
            log("WARN", "preflight.transient_failure", sha=target,
                checks=sorted(failed))
            return EXIT_RUNTIME
        failures = state["setupFailures"].get(target, 0) + 1
        state["setupFailures"][target] = failures
        log("ERROR", "preflight.failed", sha=target, failures=failures,
            checks=sorted(deterministic))
        save_state(config.state_dir, state)
        if failures >= config.max_setup_retries:
            log("ERROR", "sandbox.quarantined", sha=target, failures=failures)
            state["setupFailures"].pop(target, None)
            state["pendingSha"] = None
            state["lastProcessedSha"] = target
            save_state(config.state_dir, state)
        return EXIT_RUNTIME

    try:
        produce_baseline(sandbox)
        log("INFO", "baseline.produced", sha=target)
    except Exception as exc:
        log("ERROR", "baseline.failed", sha=target, detail=str(exc))
        return EXIT_RUNTIME

    # Sandbox + baseline ready. Create the capped key and run the session.
    state["setupFailures"].pop(target, None)
    save_state(config.state_dir, state)
    log("INFO", "sandbox.ready", sha=target, detail="launching session")
    return run_session(config, state, target)


def reconcile_keys(config, state):
    """Delete keys in keysPendingDeletion and stale owned-prefix keys.

    Called at startup and between polling cycles. Returns True if no known
    live key remains, False if a deletion is still pending.
    """
    import openrouter_keys as ork

    client = ork.OpenRouterClient(config.management_key)
    remaining = []
    for key_hash in list(state["keysPendingDeletion"]):
        try:
            client.delete_key(key_hash)
            log("INFO", "key.deleted", hash_prefix=key_hash[:12])
        except ork.OpenRouterAuthError as exc:
            log("ERROR", "key.delete_auth_failed", detail=str(exc))
            remaining.append(key_hash)
        except ork.OpenRouterError as exc:
            log("WARN", "key.delete_failed", hash_prefix=key_hash[:12], detail=str(exc))
            remaining.append(key_hash)
    state["keysPendingDeletion"] = remaining

    try:
        deleted = client.delete_stale_keys()
        if deleted:
            log("INFO", "stale_keys.deleted", count=deleted)
    except ork.OpenRouterAuthError as exc:
        log("ERROR", "stale_keys.auth_failed", detail=str(exc))
        return False
    except ork.OpenRouterError as exc:
        log("WARN", "stale_keys.list_failed", detail=str(exc))
        return False
    return not remaining


def startup_recovery(config, state):
    """Reconcile an interrupted session, stale keys, and old logs at startup."""
    if state.get("activeSession"):
        _recover_interrupted_session(config, state)
    reconcile_keys(config, state)
    save_state(config.state_dir, state)


def run_loop(config, state):
    """Persistent polling loop (systemd). One session at a time."""
    log("INFO", "supervisor.started", poll_interval=config.poll_interval_seconds)
    startup_recovery(config, state)
    while not _shutdown.is_set():
        try:
            run_once(config, state)
        except Exception as exc:
            log("ERROR", "poll.cycle_failed", detail=str(exc))
        # Reconcile stale keys and prune old logs between cycles.
        reconcile_keys(config, state)
        prune_logs(config, state)
        save_state(config.state_dir, state)
        log("INFO", "poll.sleep", seconds=config.poll_interval_seconds)
        # Sleep in slices so a shutdown signal is honored promptly.
        slept = 0
        while slept < config.poll_interval_seconds and not _shutdown.is_set():
            chunk = min(1.0, config.poll_interval_seconds - slept)
            time.sleep(chunk)
            slept += chunk
    log("INFO", "supervisor.stopped")
    return EXIT_OK


def cleanup_stale_keys(config, state):
    """--cleanup-stale-keys: reconcile without launching Hermes."""
    log("INFO", "cleanup_stale_keys.started")
    ok = reconcile_keys(config, state)
    save_state(config.state_dir, state)
    if ok:
        log("INFO", "cleanup_stale_keys.ok")
        return EXIT_OK
    log("ERROR", "cleanup_stale_keys.incomplete",
        pending=len(state["keysPendingDeletion"]))
    return EXIT_RUNTIME


def _session_id(target_sha):
    return f"{utc_now().strftime('%Y%m%dT%H%M%SZ')}-{target_sha[:8]}"


def _proc_start_ticks(pid):
    """Linux process start time (field 22 of /proc/<pid>/stat) for identity checks."""
    try:
        with open(f"/proc/{pid}/stat") as fh:
            after_comm = fh.read().rsplit(") ", 1)[1].split()
        return int(after_comm[19])
    except (OSError, IndexError, ValueError):
        return None


def _group_alive(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def _recover_interrupted_session(config, state):
    """Handle a session recorded as active when the supervisor was not running.

    Validates process identity before signaling, terminates surviving
    descendants, deletes the stored key, writes an `interrupted` summary, and
    clears active state. Returns True if recovery completed cleanly.
    """
    import openrouter_keys as ork

    active = state["activeSession"]
    session_id = active["sessionId"]
    pgid = active.get("processGroupId")
    warnings = []

    if pgid and _group_alive(pgid):
        pid = active.get("pid")
        stored_ticks = active.get("pidStartTicks")
        signal_ok = False
        if pid and stored_ticks is not None:
            if _proc_start_ticks(pid) == stored_ticks:
                signal_ok = True
            else:
                warnings.append("stored pid identity mismatch; not signaling")
        if signal_ok:
            log("WARN", "session.recovering", session=session_id, pgid=pgid)
            _terminate_group(pgid, config.termination_grace_seconds,
                             use_helper=bool(config.agent_user))
        else:
            warnings.append(f"process group {pgid} alive but identity unverified")

    # Delete the stored key hash (safe: keyed by hash, no plaintext needed).
    key_hash = active.get("keyHash")
    key_deleted = False
    if key_hash:
        try:
            client = ork.OpenRouterClient(config.management_key)
            client.delete_key(key_hash)
            key_deleted = True
            log("INFO", "key.deleted", session=session_id, hash_prefix=key_hash[:12],
                recovered=True)
        except ork.OpenRouterError as exc:
            state["keysPendingDeletion"].append(key_hash)
            warnings.append(f"key deletion failed: {exc}")
            log("ERROR", "key.delete_failed", session=session_id, detail=str(exc))

    # Write the interrupted summary.
    started_at = utc_now()
    try:
        started_at = datetime.strptime(active["startedAt"], "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=timezone.utc)
    except (KeyError, ValueError):
        pass
    ended_at = utc_now()
    try:
        _write_summary(config, active, "interrupted", -1, None, "", {},
                       warnings + ["supervisor restart detected"], started_at, ended_at)
    except OSError as exc:
        log("ERROR", "summary.write_failed", session=session_id, detail=str(exc))

    state["activeSession"] = None
    save_state(config.state_dir, state)
    log("INFO", "session.recovered", session=session_id, key_deleted=key_deleted)
    return key_deleted


def prune_logs(config, state):
    """Remove session logs older than LOG_RETENTION_DAYS.

    Only runs when no session is active. Never touches state files.
    """
    if state.get("activeSession"):
        return 0
    cutoff = time.time() - config.log_retention_days * 86400
    removed = 0
    try:
        names = os.listdir(config.session_log_dir)
    except OSError:
        return 0
    for name in names:
        if not (name.endswith(".log") or name.endswith(".json")):
            continue
        path = os.path.join(config.session_log_dir, name)
        try:
            if os.path.getmtime(path) < cutoff:
                os.unlink(path)
                removed += 1
        except OSError:
            pass
    if removed:
        log("INFO", "logs.pruned", count=removed,
            retention_days=config.log_retention_days)
    return removed


def _agent_home(config):
    """Home directory for the agent process (its own home in two-user mode)."""
    if not config.agent_user:
        return os.path.expanduser("~")
    try:
        import pwd
        return pwd.getpwnam(config.agent_user).pw_dir
    except (KeyError, ImportError, OSError):
        log("WARN", "agent.home_lookup_failed", user=config.agent_user)
        return os.path.expanduser("~")


def _session_mcp_env(config, target_sha, session_id, result_path):
    """Variables the MCP server needs, delivered through the config env block.

    Hermes filters the MCP child environment, so arbitrary parent variables do
    NOT reach mcp_server.py. The supported channel is `mcp_servers.gengin.env`
    in the per-session config.yaml, where ${VAR} expands from the Hermes
    process environment (which _build_session_env populates).
    """
    session_env = {
        "GENGIN_TARGET_SHA": target_sha,
        "GENGIN_REPO_URL": config.gengin_repo_url,
        "GENGIN_TARGET_BRANCH": config.watch_branch,
        "GENGIN_SESSION_ID": session_id,
        "GENGIN_SESSION_RESULT_PATH": result_path,
    }
    if config.gengin_inputs_dir and os.path.isdir(config.gengin_inputs_dir):
        session_env["GENGIN_INPUTS_DIR"] = config.gengin_inputs_dir
    if os.environ.get("GITHUB_TOKEN"):
        session_env["GITHUB_TOKEN"] = os.environ["GITHUB_TOKEN"]
    return session_env


def _build_session_env(config, session_id, hermes_home, query_file, usage_file,
                       inference_key, result_path):
    """Fresh environment allowlist for the Hermes child.

    Contains only required values; explicitly excludes the management key and
    unrelated secrets. The GENGIN_*/GITHUB_TOKEN entries are also the expansion
    sources for the per-session config.yaml env block.
    """
    env = {
        "HOME": _agent_home(config),
        "PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"),
        "HERMES_HOME": hermes_home,
        "OPENROUTER_API_KEY": inference_key,
        "DISPLAY": config.gengin_display,
        "LIBGL_ALWAYS_SOFTWARE": config.libgl_always_software,
        "PYTHONUNBUFFERED": "1",
        "GENGIN_TARGET_SHA": "",  # set by caller
        "GENGIN_REPO_URL": config.gengin_repo_url,
        "GENGIN_TARGET_BRANCH": config.watch_branch,
        "GENGIN_SESSION_ID": session_id,
        "GENGIN_SESSION_RESULT_PATH": result_path,
    }
    if config.gengin_inputs_dir and os.path.isdir(config.gengin_inputs_dir):
        env["GENGIN_INPUTS_DIR"] = config.gengin_inputs_dir
    if os.environ.get("GITHUB_TOKEN"):
        env["GITHUB_TOKEN"] = os.environ["GITHUB_TOKEN"]
    return env


def _launch_command(config, hermes_home, query_file, usage_file):
    """Command that starts one supervised Hermes session.

    Two-user mode goes through the fixed root-owned sudo helper (validated
    paths, no shell); single-user mode runs the project launcher directly.
    """
    if config.agent_user:
        return ["sudo", "-n", "-u", config.agent_user, AGENT_LAUNCH_HELPER,
                REPO_ROOT, hermes_home, query_file, usage_file,
                config.openrouter_model]
    launcher = os.path.join(LLMOPT_DIR, "scripts", "gengin-opt.sh")
    return [launcher, "openrouter", "--supervised",
            "--model", config.openrouter_model,
            "--query-file", query_file, "--usage-file", usage_file]


def _stream_output(proc, log_handle, secrets, stop_event):
    """Drain merged stdout+stderr, redact secrets, write to log + stdout."""
    for line in iter(proc.stdout.readline, b""):
        if stop_event.is_set() and proc.poll() is not None:
            break
        text = line.decode("utf-8", errors="replace")
        text = redact(text, secrets)
        sys.stdout.write(text)
        sys.stdout.flush()
        log_handle.write(text)
        log_handle.flush()
    proc.stdout.close()


def _helper_signal(sig_name, pgid):
    """Signal a process group via the root helper. Returns rc, or None if the
    helper itself failed to run (missing sudoers entry, sudo error, timeout).

    The two-user model runs the agent under a different UID; llmopt-supervisor
    cannot signal it directly, so termination must go through this helper.
    """
    try:
        proc = subprocess.run(
            ["sudo", "-n", AGENT_KILL_HELPER, sig_name, str(pgid)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        return proc.returncode
    except (OSError, subprocess.TimeoutExpired):
        return None


def _terminate_group(pgid, grace_seconds, use_helper=False):
    """SIGTERM the process group, wait, then SIGKILL if anything remains.

    In two-user mode (use_helper) signals and liveness probes are routed
    through the root helper, since the supervisor cannot signal the agent UID.
    """
    if use_helper:
        _helper_signal("TERM", pgid)
    else:
        try:
            os.killpg(pgid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            return
    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        if use_helper:
            if _helper_signal("probe", pgid) != 0:
                return
        else:
            try:
                os.killpg(pgid, 0)  # probe
            except (ProcessLookupError, PermissionError):
                return
        time.sleep(0.2)
    if use_helper:
        _helper_signal("KILL", pgid)
    else:
        try:
            os.killpg(pgid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def _rel(path):
    """Relativize a path under the repo root for summaries; pass through otherwise."""
    if not path:
        return ""
    try:
        return os.path.relpath(path, REPO_ROOT)
    except ValueError:
        return path


def _write_summary(config, session, exit_reason, exit_code, usage_usd, pr_url,
                   setup_checks, warnings, started_at, ended_at):
    summary = {
        "schemaVersion": 1,
        "sessionId": session["sessionId"],
        "targetSha": session["targetSha"],
        "targetBranch": config.watch_branch,
        "model": config.openrouter_model,
        "budgetUsd": float(config.openrouter_budget_usd),
        "reportedUsageUsd": usage_usd,
        "startedAt": started_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "endedAt": ended_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "durationSeconds": int((ended_at - started_at).total_seconds()),
        "exitReason": exit_reason,
        "exitCode": exit_code,
        "prUrl": pr_url,
        "inputManifestSha256": session.get("inputManifestSha256", ""),
        "environmentFingerprint": session.get("environmentFingerprint", ""),
        "usageFile": _rel(session.get("usagePath", "")),
        "logFile": _rel(session.get("logPath", "")),
        "setupChecks": setup_checks,
        "warnings": warnings,
    }
    path = session["logPath"].replace(".log", ".json")
    tmp = path + f".tmp.{os.getpid()}"
    with open(tmp, "w") as fh:
        json.dump(summary, fh, indent=2)
        fh.write("\n")
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)
    return path


def run_session(config, state, target_sha):
    """Create a capped key, launch Hermes, supervise to completion, clean up.

    Returns the exit code. Persists state through each phase.
    """
    import openrouter_keys as ork
    import main as gengin_main

    session_id = _session_id(target_sha)
    run_dir = os.path.join(config.run_dir, session_id)
    hermes_home = os.path.join(run_dir, "hermes")
    query_file = os.path.join(run_dir, "query.md")
    usage_file = os.path.join(run_dir, "usage.json")
    result_path = os.path.join(run_dir, "result.json")
    log_path = os.path.join(config.session_log_dir, f"{session_id}.log")
    os.makedirs(run_dir, exist_ok=True)
    os.makedirs(hermes_home, exist_ok=True)
    os.makedirs(config.session_log_dir, exist_ok=True)

    # Build the per-session Hermes home from the template (no credentials).
    template = os.path.join(LLMOPT_DIR, "hermes", "config.yaml.template")
    if not os.path.isfile(template):
        log("ERROR", "session.template_missing", session=session_id, path=template)
        return EXIT_RUNTIME
    with open(template) as fh:
        cfg_text = fh.read()
    cfg_text = (cfg_text.replace("__LLMOPT_DIR__", LLMOPT_DIR)
                      .replace("__PYTHON__", config.mcp_python())
                      .replace("__LOCAL_MODEL__", "local-model")
                      .replace("__DISPLAY__", config.gengin_display))
    # Hermes filters the MCP child environment; the session variables must be
    # delivered through the config env block (${VAR} expands from the Hermes
    # process environment built by _build_session_env).
    if "# __SESSION_ENV__" not in cfg_text:
        log("ERROR", "session.config_no_marker", session=session_id,
            detail="config.yaml.template lacks the __SESSION_ENV__ marker; "
                   "refusing to run without session variables")
        return EXIT_RUNTIME
    env_lines = "\n".join(
        f'      {key}: "${{{key}}}"'
        for key in _session_mcp_env(config, target_sha, session_id, result_path))
    cfg_text = cfg_text.replace("# __SESSION_ENV__", "\n" + env_lines)
    config_path = os.path.join(hermes_home, "config.yaml")
    with open(config_path, "w") as fh:
        fh.write(cfg_text)
    if config.agent_user:
        # Group-shared (setgid) dirs: the agent must read config/query and write
        # usage/result/session artifacts; the supervisor owns the directory.
        # The setgid bit makes files created here inherit the shared group.
        for path, mode in ((run_dir, 0o2770), (hermes_home, 0o2770),
                           (config_path, 0o640)):
            try:
                os.chmod(path, mode)
            except OSError:
                pass
    else:
        os.chmod(config_path, 0o600)
        os.chmod(run_dir, 0o700)

    # Generate the session query (prompt + session context block).
    prompt = os.path.join(LLMOPT_DIR, "prompts", "optimize.md")
    with open(prompt) as fh:
        prompt_text = fh.read()
    deadline_at = utc_now() + _timedelta(seconds=config.session_timeout_seconds)
    query = prompt_text + "\n\n---\n" + (
        f"Session ID: {session_id}\n"
        f"Target branch: {config.watch_branch}\n"
        f"Target commit: {target_sha}\n"
        f"Deadline UTC: {deadline_at.strftime('%Y-%m-%dT%H:%M:%SZ')}\n"
        f"Sandbox: llmOpt/gengin, already prepared at the target SHA\n\n"
        "Do not pull or checkout a different base commit. Open at most one focused "
        "pull request. If no safe measurable optimization is found, leave the "
        "sandbox clean, state that conclusion, and exit. Never merge a pull request.\n"
        "Call report_session_result exactly once before exit.\n"
    )
    with open(query_file, "w") as fh:
        fh.write(query)
    if config.agent_user:
        try:
            os.chmod(query_file, 0o640)
        except OSError:
            pass

    # The session log must exist before a key is created.
    log_handle = open(log_path, "w", encoding="utf-8")

    client = ork.OpenRouterClient(config.management_key)
    expires_at = ork.key_expires_at(config.session_timeout_seconds,
                                    config.key_expiry_grace_seconds)
    try:
        inference_key, key_hash = client.create_key(
            session_id, target_sha[:8], config.openrouter_budget_usd, expires_at)
    except ork.OpenRouterError as exc:
        log("ERROR", "key.create_failed", session=session_id, detail=str(exc))
        log_handle.close()
        return EXIT_RUNTIME

    # Phase: key_created.
    started_at = utc_now()
    state["activeSession"] = {
        "sessionId": session_id,
        "targetSha": target_sha,
        "keyHash": key_hash,
        "phase": "key_created",
        "startedAt": started_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "deadlineAt": deadline_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "pid": None,
        "processGroupId": None,
        "logPath": log_path,
        "usagePath": usage_file,
        "inputManifestSha256": _input_manifest_hash(config),
        "environmentFingerprint": _safe_fingerprint(),
    }
    save_state(config.state_dir, state)
    log("INFO", "key.created", session=session_id, hash_prefix=key_hash[:12])

    secrets = [inference_key, config.management_key]
    exit_reason = "completed"
    exit_code = 0
    usage_usd = None
    pr_url = ""
    warnings = []

    try:
        # Phase: launching.
        state["activeSession"]["phase"] = "launching"
        save_state(config.state_dir, state)

        env = _build_session_env(config, session_id, hermes_home, query_file,
                                 usage_file, inference_key, result_path)
        env["GENGIN_TARGET_SHA"] = target_sha
        cmd = _launch_command(config, hermes_home, query_file, usage_file)
        log("INFO", "agent.launching", session=session_id,
            mode="helper" if config.agent_user else "launcher")

        proc = subprocess.Popen(
            cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True, cwd=LLMOPT_DIR)

        # Phase: running.
        state["activeSession"]["phase"] = "running"
        state["activeSession"]["pid"] = proc.pid
        state["activeSession"]["processGroupId"] = proc.pid
        state["activeSession"]["pidStartTicks"] = _proc_start_ticks(proc.pid)
        save_state(config.state_dir, state)
        # Mark the SHA processed now that Hermes has started.
        state["lastProcessedSha"] = target_sha
        state["pendingSha"] = None
        save_state(config.state_dir, state)
        log("INFO", "agent.started", session=session_id, pid=proc.pid)

        stop_event = threading.Event()
        stream_thread = threading.Thread(
            target=_stream_output, args=(proc, log_handle, secrets, stop_event),
            daemon=True)
        stream_thread.start()

        # Monitor loop: deadline (monotonic) + budget polling + shutdown.
        deadline_mono = time.monotonic() + config.session_timeout_seconds
        budget_exhausted = False
        while proc.poll() is None:
            if _shutdown.is_set():
                exit_reason = "operator_shutdown"
                log("WARN", "agent.terminating", session=session_id,
                    reason="operator_shutdown")
                break
            if time.monotonic() >= deadline_mono:
                exit_reason = "timeout"
                log("WARN", "agent.terminating", session=session_id, reason="timeout")
                break
            # budget poll
            try:
                status = client.get_key_status(inference_key)
                remaining = status.get("limit_remaining")
                usage = status.get("usage")
                if remaining is not None and float(remaining) <= 0:
                    budget_exhausted = True
                if usage is not None:
                    usage_usd = float(usage)
                    if float(usage) >= float(config.openrouter_budget_usd):
                        budget_exhausted = True
            except ork.OpenRouterError:
                pass  # transient; keep running until deadline
            if budget_exhausted:
                exit_reason = "budget_exhausted"
                log("WARN", "agent.terminating", session=session_id,
                    reason="budget_exhausted", usage=usage_usd)
                break
            # Wait up to BUDGET_POLL_SECONDS (capped by the deadline), staying
            # responsive to child exit and shutdown signals.
            remaining = deadline_mono - time.monotonic()
            budget_wait = min(config.budget_poll_seconds, max(0.25, remaining))
            waited = 0.0
            while waited < budget_wait and not _shutdown.is_set():
                if proc.poll() is not None:
                    break
                time.sleep(0.25)
                waited += 0.25

        if proc.poll() is None:
            _terminate_group(proc.pid, config.termination_grace_seconds,
                             use_helper=bool(config.agent_user))
        proc.wait()
        exit_code = proc.returncode
        stop_event.set()
        stream_thread.join(timeout=5)

        if exit_reason == "completed" and exit_code != 0:
            exit_reason = "agent_failed"

        # Read the structured result artifact if present.
        if os.path.isfile(result_path):
            try:
                with open(result_path) as fh:
                    result = json.load(fh)
                if result.get("status") == "pr_created":
                    pr_url = result.get("prUrl", "")
                    exit_reason = "completed"
                elif result.get("status") == "no_change":
                    exit_reason = "no_change"
                elif result.get("status") == "blocked":
                    exit_reason = "agent_failed"
            except (OSError, json.JSONDecodeError):
                warnings.append("result artifact unreadable")

        # Final usage.
        try:
            status = client.get_key_status(inference_key)
            if status.get("usage") is not None:
                usage_usd = float(status["usage"])
        except ork.OpenRouterError:
            pass

    except Exception as exc:
        exit_reason = "agent_failed"
        warnings.append(f"supervisor exception: {exc}")
        log("ERROR", "agent.exception", session=session_id, detail=str(exc))
    finally:
        # Phase: cleanup. Delete the key, then write the summary.
        state["activeSession"]["phase"] = "cleanup_pending"
        save_state(config.state_dir, state)
        try:
            client.delete_key(key_hash)
            log("INFO", "key.deleted", session=session_id, hash_prefix=key_hash[:12])
        except ork.OpenRouterError as exc:
            state["keysPendingDeletion"].append(key_hash)
            log("ERROR", "key.delete_failed", session=session_id, detail=str(exc))
        ended_at = utc_now()
        _write_summary(config, state["activeSession"], exit_reason, exit_code,
                       usage_usd, pr_url, {}, warnings, started_at, ended_at)
        log("INFO", "session.finished", session=session_id, reason=exit_reason,
            exit_code=exit_code, usage=usage_usd)
        state["activeSession"] = None
        save_state(config.state_dir, state)
        log_handle.close()

    return EXIT_OK if exit_reason in ("completed", "no_change") else EXIT_RUNTIME


def _input_manifest_hash(config):
    manifest = os.path.join(config.gengin_inputs_dir, "manifest.json")
    if os.path.isfile(manifest):
        try:
            with open(manifest) as fh:
                return __import__("hashlib").sha256(
                    json.dumps(json.load(fh), sort_keys=True).encode()).hexdigest()
        except (OSError, json.JSONDecodeError):
            pass
    return ""


def _safe_fingerprint():
    try:
        import main as gengin_main
        old = gengin_main.PROJECT_DIR
        gengin_main.PROJECT_DIR = os.path.join(REPO_ROOT, "llmOpt", "gengin")
        try:
            return gengin_main.environmentFingerprint()
        finally:
            gengin_main.PROJECT_DIR = old
    except Exception:
        return ""


def _timedelta(**kwargs):
    from datetime import timedelta
    return timedelta(**kwargs)


def main(argv):
    modes = [arg for arg in argv if arg in ("run", "--preflight", "--once", "--dry-run",
                                            "--status", "--cleanup-stale-keys")]
    if len(modes) != 1:
        print(__doc__, file=sys.stderr)
        return EXIT_CONFIG
    mode = modes[0]
    _install_signal_handlers()

    require_key = mode in ("run", "--once", "--preflight", "--cleanup-stale-keys")
    config, errors = load_config(require_management_key=require_key)
    if errors:
        for error in errors:
            print(f"config error: {error}", file=sys.stderr)
        return EXIT_CONFIG

    # Helper subprocesses (make bench, glxinfo, profiling) need the same
    # display the sessions use; systemd provides no DISPLAY. Set it before any
    # module that defaults Display on its own (main.py defaults to :2).
    os.environ["DISPLAY"] = config.gengin_display
    os.environ["LIBGL_ALWAYS_SOFTWARE"] = config.libgl_always_software

    if mode == "--dry-run":
        state = load_state(config.state_dir)
        print_dry_run(config, state)
        return EXIT_OK

    if mode == "--status":
        state = load_state(config.state_dir)
        print_status(config, state)
        return EXIT_OK

    lock = SupervisorLock(config.state_dir)
    try:
        lock.acquire()
    except LockHeldError as exc:
        log("INFO", "lock.held", owner_pid=exc.owner_pid)
        return EXIT_OK

    try:
        state = load_state(config.state_dir)
        if mode == "--preflight":
            sandbox = os.path.realpath(os.path.join(REPO_ROOT, "llmOpt", "gengin"))
            if not os.path.isdir(sandbox):
                log("WARN", "preflight.no_sandbox",
                    detail="llmOpt/gengin missing; build/bench/asset checks skipped")
                sandbox = None
            code, _results = run_preflight(config, sandbox=sandbox)
            return code
        if mode == "--once":
            startup_recovery(config, state)
            return run_once(config, state)
        if mode == "run":
            return run_loop(config, state)
        if mode == "--cleanup-stale-keys":
            return cleanup_stale_keys(config, state)
    finally:
        lock.release()
    return EXIT_OK


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except StateCorruptError as exc:
        log("ERROR", "state.corrupt", detail=str(exc))
        sys.exit(EXIT_STATE_CORRUPT)
    except KeyboardInterrupt:
        log("WARN", "supervisor.interrupted")
        sys.exit(EXIT_RUNTIME)
