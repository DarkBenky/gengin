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
  if grep -q 'DSH_LAUNCH_TOKEN' "$bundle"; then
    return 0
  fi
  needle='const created = encodeBase64Url(randomBytes(SECRET_BYTES));'
  if ! grep -qF "$needle" "$bundle"; then
    echo "warning: token patch point not found (dsh updated?) - login token stays per-run" >&2
    return 0
  fi
  backup="$bundle.orig-backup"
  [[ -f "$backup" ]] || cp "$bundle" "$backup"
  python3 - "$bundle" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = "const created = encodeBase64Url(randomBytes(SECRET_BYTES));"
new = "const created = process.env.DSH_LAUNCH_TOKEN || encodeBase64Url(randomBytes(SECRET_BYTES));"
open(path, "w").write(src.replace(old, new))
PY
  echo "web login: patched $bundle for a stable token"
}
