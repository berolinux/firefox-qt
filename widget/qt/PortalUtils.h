/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_qt_PortalUtils_h
#define mozilla_widget_qt_PortalUtils_h

namespace mozilla::widget {

// Matches widget/gtk/WidgetUtilsGtk PortalKind (subset + Settings).
enum class PortalKind {
  FilePicker,
  MimeHandler,
  NativeMessaging,
  Settings,
  Location,
  OpenUri,
};

// 0 never, 1 always, 2 auto (sandbox / QT_USE_PORTAL / GTK_USE_PORTAL).
// Auto defaults differ by kind (same policy as GTK WidgetUtilsGtk).
bool ShouldUsePortal(PortalKind aKind);

bool IsRunningUnderFlatpakOrSnap();

}  // namespace mozilla::widget

#endif
