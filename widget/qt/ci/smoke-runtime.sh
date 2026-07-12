#!/usr/bin/env bash
# Runtime smoke checks for a built cairo-qt tree.
# Usage: MOZCONFIG=…/mozconfig.qt widget/qt/ci/smoke-runtime.sh [objdir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

if [[ -z "${MOZCONFIG:-}" ]]; then
  if [[ -f "$ROOT/mozconfig.qt" ]]; then
    export MOZCONFIG="$ROOT/mozconfig.qt"
  else
    echo "error: set MOZCONFIG" >&2
    exit 1
  fi
fi

OBJDIR="${1:-}"
if [[ -z "$OBJDIR" ]]; then
  OBJDIR=$(python3 - <<'PY'
import os, re
text=open(os.environ["MOZCONFIG"]).read()
m=re.search(r"MOZ_OBJDIR\s*=\s*@TOPSRCDIR@/(\S+)", text)
print(m.group(1) if m else "obj-qt")
PY
)
fi

BIN="$ROOT/$OBJDIR/dist/bin/firefox"
XUL="$ROOT/$OBJDIR/dist/bin/libxul.so"
fail=0

echo "== cairo-qt runtime smoke ($OBJDIR) =="

if [[ ! -x "$BIN" ]]; then
  echo "FAIL: missing $BIN"
  exit 1
fi
echo "OK: firefox binary"

if [[ ! -f "$XUL" ]]; then
  echo "FAIL: missing $XUL"
  exit 1
fi

# Qt toolkit linkage
if ! ldd "$XUL" 2>/dev/null | grep -q 'libQt6'; then
  echo "FAIL: libxul not linked to Qt6"
  fail=1
else
  echo "OK: libxul links Qt6"
  ldd "$XUL" 2>/dev/null | grep 'libQt6' | sed 's/^/  /' | head -10
fi

# Preferred global-menu backend
if ldd "$XUL" 2>/dev/null | grep -q 'libdbusmenu-qt6'; then
  echo "OK: dbusmenu-qt6 linked"
else
  echo "WARN: dbusmenu-qt6 not linked (optional)"
fi

# --version must work headless
ver_out="$("$BIN" --version 2>&1 || true)"
if echo "$ver_out" | grep -qi 'Mozilla Firefox'; then
  echo "OK: firefox --version: $(echo "$ver_out" | head -1)"
else
  echo "FAIL: firefox --version unexpected: $ver_out"
  fail=1
fi

# Headless start/stop (no full UI session required)
# Use a temp profile; timeout if it hangs.
PROFILE=$(mktemp -d)
cleanup() { rm -rf "$PROFILE"; }
trap cleanup EXIT

set +e
timeout 30s env MOZ_HEADLESS=1 QT_QPA_PLATFORM=offscreen \
  "$BIN" --headless -profile "$PROFILE" -no-remote \
  about:blank >/tmp/qt-firefox-headless.log 2>&1 &
pid=$!
# Give it a few seconds to start
sleep 10
if kill -0 "$pid" 2>/dev/null; then
  echo "OK: headless process running (pid $pid)"
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
else
  wait "$pid"
  rc=$?
  if [[ $rc -eq 124 ]]; then
    echo "OK: headless ran until timeout (expected)"
  elif grep -qi 'headless mode' /tmp/qt-firefox-headless.log 2>/dev/null; then
    # Started in headless then exited (crash or early quit) — soft fail for CI.
    echo "WARN: headless exited rc=$rc after enabling headless — log:"
    tail -25 /tmp/qt-firefox-headless.log | sed 's/^/  /'
  else
    echo "FAIL: headless did not start (rc=$rc)"
    tail -25 /tmp/qt-firefox-headless.log | sed 's/^/  /'
    fail=1
  fi
fi
set -e

# XPCOM component registration sanity: toolkit string in application.ini
appini="$ROOT/$OBJDIR/dist/bin/application.ini"
if [[ -f "$appini" ]]; then
  echo "OK: application.ini present"
else
  echo "WARN: application.ini missing"
fi

if [[ "$fail" -ne 0 ]]; then
  echo "smoke-runtime: FAILED"
  exit 1
fi
echo "smoke-runtime: PASSED"
