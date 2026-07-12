/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PortalUtils.h"

#include "mozilla/StaticPrefs_widget.h"
#include "prenv.h"

#include <cstdlib>

namespace mozilla::widget {

bool IsRunningUnderFlatpakOrSnap() {
  return PR_GetEnv("FLATPAK_ID") || PR_GetEnv("SNAP") ||
         PR_GetEnv("SNAP_NAME");
}

static bool PortalEnvEnabled() {
  if (IsRunningUnderFlatpakOrSnap()) {
    return true;
  }
  if (const char* e = PR_GetEnv("QT_USE_PORTAL")) {
    return atoi(e) != 0;
  }
  if (const char* e = PR_GetEnv("GTK_USE_PORTAL")) {
    return atoi(e) != 0;
  }
  return false;
}

bool ShouldUsePortal(PortalKind aKind) {
  bool autoBehavior = PortalEnvEnabled();
  int32_t pref = 2;
  switch (aKind) {
    case PortalKind::FilePicker:
      // Prefer portal when available (same as GTK: auto defaults true).
      autoBehavior = true;
      pref = StaticPrefs::widget_use_xdg_desktop_portal_file_picker();
      // For non-sandbox, auto still needs env or we always-try then fallback.
      // GTK sets autoBehavior=true meaning "try portal". Outside sandbox that
      // can mean always — keep true for Qt too (file picker falls back).
      break;
    case PortalKind::MimeHandler:
      // Breaks default-browser flow outside sandbox (bug 1516290).
      autoBehavior = IsRunningUnderFlatpakOrSnap();
      pref = StaticPrefs::widget_use_xdg_desktop_portal_mime_handler();
      break;
    case PortalKind::NativeMessaging:
      // Auto: sandboxes / QT_USE_PORTAL (QtDBus WebExtensions portal).
      autoBehavior = PortalEnvEnabled();
      pref = StaticPrefs::widget_use_xdg_desktop_portal_native_messaging();
      break;
    case PortalKind::Settings:
      autoBehavior = true;
      pref = StaticPrefs::widget_use_xdg_desktop_portal_settings();
      break;
    case PortalKind::Location:
      autoBehavior = PortalEnvEnabled();
      pref = StaticPrefs::widget_use_xdg_desktop_portal_location();
      break;
    case PortalKind::OpenUri:
      autoBehavior = PortalEnvEnabled();
      pref = StaticPrefs::widget_use_xdg_desktop_portal_open_uri();
      break;
  }
  switch (pref) {
    case 0:
      return false;
    case 1:
      return true;
    default:
      return autoBehavior;
  }
}

}  // namespace mozilla::widget
