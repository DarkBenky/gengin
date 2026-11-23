#!/usr/bin/env bash
# Shared dsh Web UI login-token handling. Sourced by dsh-web-local.sh (the
# model-only UI on :3080) and gengin-opt.sh (the per-run optimizer UI on
# :3081). dsh mints a random launch token per process, so every restart would
# break bookmarks and force re-authentication from the journal;
# dshWebPrepareLaunch pins one token (llmOpt/.dsh/web-launch-token) and
# idempotently patches the installed dsh-client-connection bundle to read
# DSH_LAUNCH_TOKEN. The patch is re-applied on every run, so a dsh update
# self-heals. Never fatal: when the bundle or the patch point is missing the
# caller continues with dsh's per-run token.
#
# The same pass removes the browser auth gate (isAuthenticated always true) so
# the UI opens without a bookmark. The server still binds 127.0.0.1 only, so
# this is a local-trust decision: any local process can drive the agent.
# DSH_WEB_NO_AUTH=0 keeps dsh's auth (and restores it from the backup).
#
# For a LAN bind (DSH_WEB_HOST=lan) two more patches are applied, both of which
# dsh blocks on purpose:
#   - dsh refuses --host 0.0.0.0 in its CLI, while its webserver schema accepts
#     nothing else non-loopback, so the refusal is what stands in the way;
#   - the /api Host fence only trusts the addresses dsh snapshotted when the
#     server bound. A Wi-Fi/DHCP change then turns every /api call into 403 -
#     the page loads but shows no models, workspaces or sessions - so the fence
#     also accepts any IP-literal host. DNS rebinding needs an attacker-owned
#     NAME, so hostnames stay fenced.
#
# A patch point that a dsh update moves prints a warning and the caller keeps
# dsh's own behaviour; every patched file is re-parsed with `node --check` and
# restored if it does not parse, so a bad patch can never brick the service.

# dshWebPrepareLaunch <dsh-home>: ensure the token file, patch the bundle,
# export DSH_LAUNCH_TOKEN.
dshWebPrepareLaunch() {
  local dsh_dir="$1" token_file="$1/web-launch-token" dsh_root bundle backup needle
  if [[ ! -s "$token_file" ]]; then
    python3 -c 'import secrets; print(secrets.token_urlsafe(32))' > "$token_file"
    chmod 600 "$token_file"
  fi
  export DSH_LAUNCH_TOKEN="$(cat "$token_file")"

  dsh_root="$(cd "$(dirname "$(command -v dsh)")/.." && pwd)"
  bundle="$dsh_root/lib/node_modules/@deepseek-ai/dsh/node_modules/@deepseek-ai/dsh-client-connection/lib/index.js"
  [[ -f "$bundle" ]] || bundle="$(find "$dsh_root" -type f -path '*dsh-client-connection/lib/index.js' 2>/dev/null | head -1 || true)"
  if [[ -z "$bundle" || ! -f "$bundle" ]]; then
    echo "warning: dsh-client-connection bundle not found - login token stays per-run" >&2
    return 0
  fi
  backup="$bundle.orig-backup"
  [[ -f "$backup" ]] || cp "$bundle" "$backup"

  if ! grep -q 'DSH_LAUNCH_TOKEN' "$bundle"; then
    needle='const created = encodeBase64Url(randomBytes(SECRET_BYTES));'
    if ! grep -qF "$needle" "$bundle"; then
      echo "warning: token patch point not found (dsh updated?) - login token stays per-run" >&2
    else
      python3 - "$bundle" <<'PY' || echo "warning: token patch failed - login token stays per-run" >&2
import sys
path = sys.argv[1]
src = open(path).read()
old = "const created = encodeBase64Url(randomBytes(SECRET_BYTES));"
new = "const created = process.env.DSH_LAUNCH_TOKEN || encodeBase64Url(randomBytes(SECRET_BYTES));"
open(path, "w").write(src.replace(old, new))
PY
      echo "web login: patched $bundle for a stable token"
    fi
  fi

  # Auth gate: the index handler and every RPC admission call isAuthenticated,
  # so making it unconditional removes both. Brace-matched, so whitespace or
  # statement changes inside the function do not matter.
  if [[ "${DSH_WEB_NO_AUTH:-1}" == "0" ]]; then
    if grep -q 'dsh-web-no-auth' "$bundle"; then
      cp "$backup" "$bundle"
      echo "web login: auth restored from $backup"
    fi
    return 0
  fi
  if ! grep -q 'dsh-web-no-auth' "$bundle"; then
    if ! grep -q 'isAuthenticated(request) {' "$bundle"; then
      echo "warning: isAuthenticated not found (dsh updated?) - login still required" >&2
      return 0
    fi
    python3 - "$bundle" <<'PY' || echo "warning: auth patch failed - login still required" >&2
import sys
path = sys.argv[1]
src = open(path).read()
marker = "isAuthenticated(request) {"
start = src.index(marker) + len(marker) - 1
depth = 0
for i in range(start, len(src)):
    if src[i] == "{":
        depth += 1
    elif src[i] == "}":
        depth -= 1
        if depth == 0:
            end = i
            break
else:
    raise SystemExit("unbalanced braces around isAuthenticated")
body = "\n\t\treturn true; /* dsh-web-no-auth */\n\t"
open(path, "w").write(src[:start] + "{" + body + "}" + src[end + 1:])
PY
    echo "web login: auth gate removed in $bundle"
  fi
  dshWebCheckJs "$bundle" "$backup"

  # LAN bind: dsh refuses --host 0.0.0.0 in the CLI (RCE-exposure guard) while
  # its webserver schema accepts nothing else non-loopback. Removing the guard
  # is what lets the UI answer on the network; the Host fence then derives every
  # non-internal IPv4 into trustedHosts, so http://<lan-ip>:PORT is accepted.
  dshWebPatchLanGuard "$dsh_root"
  if [[ "${DSH_WEB_HOST:-127.0.0.1}" != "127.0.0.1" ]]; then
    dshWebPatchIpLiteralTrust "$bundle"
  fi
}

# dshWebCheckJs <file> <backup>: a patch that stops the file parsing would stop
# dsh from booting at all, so restore the pre-patch copy instead.
dshWebCheckJs() {
  node --check "$1" >/dev/null 2>&1 && return 0
  cp "$2" "$1"
  echo "warning: patched $1 does not parse - restored $2 (dsh runs unpatched)" >&2
  return 0
}

# dshWebPatchIpLiteralTrust <connection-bundle>: let the /api fence accept any
# IP-literal Host, not only the addresses snapshotted at startup.
#
# dsh derives trustedHosts once, when the server binds. A machine that changes
# network (Wi-Fi -> another subnet, DHCP lease) keeps serving index.html but
# every /api call answers 403, so the UI loads with no models, no workspaces
# and no sessions. Accepting IP literals restores exactly the guarantee the
# derivation was reaching for: DNS rebinding needs an attacker-controlled NAME,
# so a literal Host is trusted on any address while hostnames stay fenced.
dshWebPatchIpLiteralTrust() {
  local bundle="$1"
  grep -q 'dsh-web-any-ip' "$bundle" && return 0
  python3 - "$bundle" <<'PY' || echo "warning: Host-fence patch failed - a changed IP will 403 again" >&2
import sys
path = sys.argv[1]
src = open(path).read()

helper = (
    'function isIpLiteralHostname(hostname) { /* dsh-web-any-ip */\n'
    '\treturn /^\\[[0-9a-fA-F:.]+\\]$/.test(hostname) || /^[0-9]{1,3}(\\.[0-9]{1,3}){3}$/.test(hostname);\n'
    '}\n'
)
marker = "function isTrustedApiRequest(request, trustedHosts) {"
at = src.find(marker)
if at == -1:
    raise SystemExit("any-ip: isTrustedApiRequest not found (dsh updated?)")
src = src[:at] + helper + src[at:]

old = ("\tif (!isLoopbackHostname(hostUrl.hostname) && !isTrustedAuthority(hostUrl, "
       "trustedHosts)) return false;")
new = ("\tif (!isLoopbackHostname(hostUrl.hostname) && !isTrustedAuthority(hostUrl, "
       "trustedHosts) && !isIpLiteralHostname(hostUrl.hostname)) return false; /* dsh-web-any-ip */")
if old not in src:
    raise SystemExit("any-ip: fence condition not found (dsh updated?)")
open(path, "w").write(src.replace(old, new))
PY
  dshWebCheckJs "$bundle" "$bundle.orig-backup"
  echo "web lan: /api accepts IP-literal hosts in $(basename "$(dirname "$bundle")")"
}

# dshWebPatchLanGuard <dsh-root>: idempotently drop the 0.0.0.0 refusal.
dshWebPatchLanGuard() {
  local dsh_root="$1" startup
  startup="$(find "$dsh_root" -type f -path '*dsh-web-app/lib/startup.js' 2>/dev/null | head -1 || true)"
  if [[ -z "$startup" || ! -f "$startup" ]]; then
    echo "warning: dsh-web-app startup.js not found - LAN bind unavailable" >&2
    return 0
  fi
  grep -q 'dsh-web-lan-guard' "$startup" && return 0
  [[ -f "$startup.orig-backup" ]] || cp "$startup" "$startup.orig-backup"
  python3 - "$startup" <<'PY' || echo "warning: LAN-bind patch failed - dsh refuses 0.0.0.0" >&2
import sys
path = sys.argv[1]
src = open(path).read()
needle = 'if (options.host === "0.0.0.0") program.error('
at = src.find(needle)
if at == -1:
    raise SystemExit("lan guard: 0.0.0.0 refusal not found (dsh updated?)")
end = src.find("\n", at)
line = src[at:end]
if not line.rstrip().endswith(";"):
    raise SystemExit("lan guard: refusal statement did not end on one line")
open(path, "w").write(src[:at] + "/* dsh-web-lan-guard: 0.0.0.0 allowed */" + src[end:])
PY
  dshWebCheckJs "$startup" "$startup.orig-backup"
  echo "web lan: 0.0.0.0 bind allowed in $(basename "$(dirname "$startup")")"
}

# dshWebPatchStatus: what is currently applied, so a dsh update that moved a
# patch point shows up in the launch banner instead of as a mystery 403.
dshWebPatchStatus() {
  local dsh_root bundle startup token=no auth=on lan=no fence=boot-ips
  dsh_root="$(cd "$(dirname "$(command -v dsh)")/.." && pwd)"
  bundle="$dsh_root/lib/node_modules/@deepseek-ai/dsh/node_modules/@deepseek-ai/dsh-client-connection/lib/index.js"
  [[ -f "$bundle" ]] || bundle="$(find "$dsh_root" -type f -path '*dsh-client-connection/lib/index.js' 2>/dev/null | head -1 || true)"
  startup="$(find "$dsh_root" -type f -path '*dsh-web-app/lib/startup.js' 2>/dev/null | head -1 || true)"
  if [[ -n "$bundle" && -f "$bundle" ]]; then
    grep -q 'DSH_LAUNCH_TOKEN' "$bundle" && token=stable
    grep -q 'dsh-web-no-auth' "$bundle" && auth=off
    grep -q 'dsh-web-any-ip' "$bundle" && fence=any-ip
  fi
  [[ -n "$startup" && -f "$startup" ]] && grep -q 'dsh-web-lan-guard' "$startup" && lan=yes
  printf 'dsh patches: token=%s auth=%s lan-bind=%s host-fence=%s\n' \
    "$token" "$auth" "$lan" "$fence"
  [[ "$lan" == no && "${DSH_WEB_HOST:-127.0.0.1}" != "127.0.0.1" ]] && {
    echo "warning: LAN bind requested but the dsh guard is still in place - " >&2
    echo "  the patch point moved (dsh update?); the UI stays on 127.0.0.1" >&2
  }
  [[ "$fence" == boot-ips && "${DSH_WEB_HOST:-127.0.0.1}" != "127.0.0.1" ]] && {
    echo "warning: /api only trusts the IPs seen at startup - after a network" >&2
    echo "  change the page will load without models; restart the service then" >&2
  }
  return 0
}
