/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_QtPrefUtils_h
#define mozilla_widget_QtPrefUtils_h

#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_widget.h"

namespace mozilla::widget {

// Prefer widget.qt.*; if the user has not set it, honor widget.gtk.* so
// older user.js / docs keep working.

inline bool PrefGlobalMenuEnabled() {
  if (Preferences::HasUserValue("widget.qt.global-menu.enabled")) {
    return StaticPrefs::widget_qt_global_menu_enabled();
  }
  if (Preferences::HasUserValue("widget.gtk.global-menu.enabled")) {
    return StaticPrefs::widget_gtk_global_menu_enabled();
  }
  return StaticPrefs::widget_qt_global_menu_enabled();
}

inline bool PrefGlobalMenuWaylandEnabled() {
  if (Preferences::HasUserValue("widget.qt.global-menu.wayland.enabled")) {
    return StaticPrefs::widget_qt_global_menu_wayland_enabled();
  }
  if (Preferences::HasUserValue("widget.gtk.global-menu.wayland.enabled")) {
    return StaticPrefs::widget_gtk_global_menu_wayland_enabled();
  }
  return StaticPrefs::widget_qt_global_menu_wayland_enabled();
}

inline bool PrefNativeContextMenus() {
  if (Preferences::HasUserValue("widget.qt.native-context-menus")) {
    return StaticPrefs::widget_qt_native_context_menus();
  }
  if (Preferences::HasUserValue("widget.gtk.native-context-menus")) {
    return StaticPrefs::widget_gtk_native_context_menus();
  }
  return StaticPrefs::widget_qt_native_context_menus();
}

inline bool PrefNativeThemeControls() {
  return StaticPrefs::widget_qt_theme_native_controls_enabled();
}

}  // namespace mozilla::widget

#endif
