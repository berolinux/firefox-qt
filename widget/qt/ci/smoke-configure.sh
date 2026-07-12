#!/usr/bin/env bash
# Smoke-check a cairo-qt configure tree. Intended for packaging CI / local gates.
# Usage: MOZCONFIG=path/to/mozconfig.qt widget/qt/ci/smoke-configure.sh [objdir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

if [[ -z "${MOZCONFIG:-}" ]]; then
  if [[ -f "$ROOT/mozconfig.qt" ]]; then
    export MOZCONFIG="$ROOT/mozconfig.qt"
  else
    echo "error: set MOZCONFIG to a cairo-qt mozconfig" >&2
    exit 1
  fi
fi

OBJDIR="${1:-}"
if [[ -z "$OBJDIR" ]]; then
  # shellcheck disable=SC1090
  OBJDIR=$(python3 - <<'PY'
import os, re
path=os.environ["MOZCONFIG"]
text=open(path).read()
m=re.search(r"MOZ_OBJDIR\s*=\s*@TOPSRCDIR@/(\S+)", text)
print(m.group(1) if m else "obj-qt")
PY
)
fi

CONFIG_H="$ROOT/$OBJDIR/mozilla-config.h"
STATUS="$ROOT/$OBJDIR/config.status"

if [[ ! -f "$CONFIG_H" ]]; then
  echo "error: $CONFIG_H missing — run ./mach configure first" >&2
  exit 1
fi

fail=0
check_define() {
  local name="$1" expect="${2:-}"
  if ! grep -q "#define $name" "$CONFIG_H"; then
    echo "FAIL: missing #define $name in mozilla-config.h"
    fail=1
    return
  fi
  if [[ -n "$expect" ]]; then
    if ! grep -q "#define $name $expect" "$CONFIG_H"; then
      echo "WARN: $name present but not exactly '$expect'"
    fi
  fi
  echo "OK: $name"
}

echo "== cairo-qt configure smoke ($OBJDIR) =="
check_define MOZ_WIDGET_QT 1
check_define MOZ_ENABLE_DBUS 1

# Optional but strongly recommended on Plasma packs.
if grep -q "MOZ_ENABLE_DBUSMENU_QT 1" "$CONFIG_H"; then
  echo "OK: MOZ_ENABLE_DBUSMENU_QT 1"
else
  echo "WARN: MOZ_ENABLE_DBUSMENU_QT not set (libdbusmenu-qt6 missing at configure)"
fi

if grep -q "NECKO_WIFI_DBUS 1" "$CONFIG_H"; then
  echo "OK: NECKO_WIFI_DBUS 1"
else
  echo "WARN: NECKO_WIFI_DBUS not enabled"
fi

# Toolkit string in config.status
if [[ -f "$STATUS" ]]; then
  if grep -q "MOZ_WIDGET_TOOLKIT.: .qt" "$STATUS" || grep -q "'MOZ_WIDGET_TOOLKIT': 'qt'" "$STATUS"; then
    echo "OK: MOZ_WIDGET_TOOLKIT=qt"
  else
    echo "FAIL: MOZ_WIDGET_TOOLKIT is not qt in config.status"
    fail=1
  fi
fi

# First-party sources must not pull GIO on the Qt path.
echo "== first-party GIO scan (widget/qt, unixdesktop, shell Qt) =="
if rg -n "gio/gio\.h|g_dbus_proxy|GDBusProxy" \
    --glob '!ci/**' \
    widget/qt toolkit/system/unixdesktop \
    browser/components/shell/nsQtShellService.cpp \
    netwerk/wifi/dbus/QtDbusWifiScanner.cpp 2>/dev/null; then
  echo "FAIL: GIO symbols found in first-party Qt paths"
  fail=1
else
  echo "OK: no GIO in first-party Qt paths"
fi

if [[ "$fail" -ne 0 ]]; then
  echo "smoke-configure: FAILED"
  exit 1
fi
echo "smoke-configure: PASSED"
