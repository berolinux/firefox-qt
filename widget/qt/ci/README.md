# Qt (cairo-qt) CI helpers

Firefox upstream Taskcluster does not yet ship a `cairo-qt` build graph.
Packagers and local gates can use:

```bash
export MOZCONFIG=$PWD/mozconfig.qt
./mach configure
./widget/qt/ci/smoke-configure.sh
./mach build
```

`smoke-configure.sh` verifies:

- `MOZ_WIDGET_QT` / toolkit=`qt`
- DBus enabled
- optional dbusmenu-qt6 and NECKO_WIFI_DBUS
- first-party Qt sources do not include GIO

## Suggested packaging CI stages

1. **configure** — run `./mach configure` with the distro `mozconfig.qt`
2. **smoke-configure** — this script
3. **build** — `./mach build` (or `./mach build binaries` for faster iteration)
4. **manual Plasma checklist** — see `widget/qt/PACKAGING.md`

Wire steps 1–3 into your distro pipeline (OpenMandriva ABF, OBS, GitLab CI, etc.).

## Taskcluster

An experimental job is defined in `taskcluster/kinds/build/linux-qt.yml`
(`linux64-qt/opt`, Treeherder symbol **Bq**, tier 3).

- Mozconfig: `browser/config/mozconfigs/linux64/qt-opt`
- `run-on-projects: []` until Firefox Linux docker images ship Qt6
- Try: `./mach try fuzzy -q 'linux64-qt'`

## Runtime smoke

After a local or packager build:

```bash
export MOZCONFIG=$PWD/mozconfig.qt
./widget/qt/ci/smoke-configure.sh
./widget/qt/ci/smoke-runtime.sh
```

## Source-test (Taskcluster, no Qt SDK)

`source-test/qt-source-smoke` (Treeherder **Qt(s)**) runs when Qt-related
paths change. It checks mozconfigs, scripts, and first-party GIO absence.

Full binary builds remain `linux64-qt/opt` (try-only until docker has Qt6).
