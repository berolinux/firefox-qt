# Qt Firefox (cairo-qt) packaging notes

## Runtime dependencies (desktop)

### Core
- Qt 6.11+ (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6DBus`, `Qt6PrintSupport`)
- NetworkManager (WiFi scan over system D-Bus via QtDBus)
- Optional: GLib for dbusmenu-glib global-menu fallback and for PipeWire
  portal (libwebrtc still uses GIO internally)
- First-party Qt code uses **QtDBus only** (file portal, OpenURI, settings,
  AppMenu.Registrar, camera presence, WiFi) — no GIO
- PipeWire + session (WirePlumber) for camera/screen capture
- `xdg-desktop-portal` and a backend (`xdg-desktop-portal-kde` on Plasma,
  `xdg-desktop-portal-gnome` on GNOME)

### Global menu (Plasma / Unity-style)
- **Preferred:** `libdbusmenu-qt6`
- **Fallback:** `libdbusmenu-glib` (dlopen `libdbusmenu-glib.so.4`)
- KWin appmenu protocol (built-in) for Wayland global menu

### Optional
- `libva` / VAAPI stack for hardware video decode (already gated by build)
- KF6 packages (optional; **auto-detected at configure**, never required):
  - `KF6Service` — richer MIME / preferred app lookup
  - Disable all KF with `ac_add_options --disable-kf6` for a pure-Qt build
- Download UX needs **no KF**: Unity launcher badge + Plasma JobView use QtDBus

## Useful environment variables

| Variable | Effect |
|----------|--------|
| `QT_USE_PORTAL=1` | Prefer XDG portals for file picker / OpenURI |
| `GTK_USE_PORTAL=1` | Same (honoured by Qt portal auto path) |
| `MOZ_LOG=QtGlobalMenu:5,QtFilePicker:5` | Debug global menu / file portal |

## Prefs (Qt defaults)

- `media.webrtc.capture.allow-pipewire` = true
- `media.webrtc.camera.allow-pipewire` = true
- `widget.qt.global-menu.enabled` = true (falls back to `widget.gtk.global-menu.enabled`)
- `widget.qt.global-menu.wayland.enabled` = true (falls back to `widget.gtk.*`)
- `widget.qt.native-context-menus` = true (falls back to `widget.gtk.native-context-menus`)
- `widget.qt.theme.native-controls.enabled` = true (QStyle form controls)
- `widget.qt.downloads.launcher-count.enabled` = true (task manager badge)
- `widget.qt.downloads.jobview.enabled` = true (Plasma job tracker / QtDBus)
- `widget.use-xdg-desktop-portal.file-picker` = 2 (auto)
- `widget.use-xdg-desktop-portal.open-uri` = 2 (auto)
- `widget.use-xdg-desktop-portal.settings` = 2 (auto; color-scheme/accent)

Dual-read rule: if the user has set a `widget.qt.*` pref, that wins; otherwise
an explicit `widget.gtk.*` value is honoured for compatibility.

## Smoke checklist (Plasma)

1. **Screen share** on Wayland and X11 — portal picker appears, video frames
2. **Camera** — portal or V4L2 fallback works in getUserMedia
3. **Global menu** — enable app menu widget; File/Edit appear in panel with shortcuts
4. **File open** with `QT_USE_PORTAL=1` — portal FileChooser
5. **Open link** with `QT_USE_PORTAL=1` — portal OpenURI
6. **Context menus** — native QMenu styling, icons/shortcuts where available
7. **Notifications** — download / web notifications via FDO
8. **Taskbar progress** — download progress + badge count on icon
9. **Plasma job tracker** — download appears in notification-area jobs
10. **Set desktop background** (image + solid color) under Plasma

## Build

```bash
export MOZCONFIG=path/to/mozconfig.qt
./mach configure
./widget/qt/ci/smoke-configure.sh
./widget/qt/ci/smoke-runtime.sh
./mach build
```

Ensure configure reports `dbusmenu-qt6` and `MOZ_ENABLE_DBUS` when available.

## CI

Distro/packaging CI should run `widget/qt/ci/smoke-configure.sh` and
`widget/qt/ci/smoke-runtime.sh` (see `widget/qt/ci/README.md`).

Experimental Taskcluster job: `linux64-qt/opt` in
`taskcluster/kinds/build/linux-qt.yml` (Treeherder **Bq**, tier 3,
try-only until docker images ship Qt6).
