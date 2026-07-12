/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_qt_DBusMenuBar_h
#define mozilla_widget_qt_DBusMenuBar_h

#include "mozilla/RefCounted.h"
#include "mozilla/UniquePtr.h"
#include "nsString.h"

struct org_kde_kwin_appmenu;

namespace mozilla {

namespace dom {
class Element;
}

namespace widget {

// Exports a XUL menubar over com.canonical.dbusmenu and registers it with
// Plasma/Unity AppMenu.Registrar (X11) or org_kde_kwin_appmenu (Wayland).
//
// Preferred backend: libdbusmenu-qt6 (QMenu + DBusMenuExporter + QtDBus).
// Optional fallback: libdbusmenu-glib (dlopen) when Qt backend is unavailable.
class DBusMenuBar final : public RefCounted<DBusMenuBar> {
 public:
  MOZ_DECLARE_REFCOUNTED_TYPENAME(DBusMenuBar)

  // Prefer Qt dbusmenu; fall back to glib dbusmenu if compiled/available.
  static RefPtr<DBusMenuBar> Create(dom::Element* aMenuBarElement);

  // True if any backend can export menus (for NativeMenuSupport gating).
  static bool Available();

  // True when the KWin appmenu global is available (Wayland/Plasma).
  static bool WaylandAppMenuAvailable();

  ~DBusMenuBar();

 private:
  class Backend;
  class QtBackend;
  class GlibBackend;

  explicit DBusMenuBar(UniquePtr<Backend> aBackend);
  void TryRegisterWayland();

  UniquePtr<Backend> mBackend;
  org_kde_kwin_appmenu* mAppMenu = nullptr;
};

}  // namespace widget
}  // namespace mozilla

#endif
