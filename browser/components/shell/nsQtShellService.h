/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsQtShellService_h_
#define nsQtShellService_h_

#include "nsIUnixShellService.h"
#include "nsToolkitShellService.h"
#include "nsString.h"

/**
 * Linux/Qt shell service: FreeDesktop + Plasma integration.
 *
 * - Default browser: xdg-settings (KDE BrowserApplication) + xdg-mime
 * - Per-scheme defaults: xdg-mime / xdg-settings
 * - Wallpaper: plasma-apply-wallpaperimage when available
 * No GIO/GTK.
 */
class nsQtShellService final : public nsIUnixShellService,
                               public nsToolkitShellService {
 public:
  nsQtShellService() = default;

  NS_DECL_ISUPPORTS
  NS_DECL_NSISHELLSERVICE
  NS_DECL_NSIUNIXSHELLSERVICE

  nsresult Init();

 private:
  ~nsQtShellService() = default;

  // Desktop file id used with xdg-settings (e.g. "firefox.desktop").
  nsCString mDesktopId;

  static nsresult RunProcess(const char* aExecutable,
                             const nsTArray<nsCString>& aArgs,
                             nsACString* aStdout, int* aExitStatus);
  static nsresult RunXdgSettings(const nsTArray<nsCString>& aArgs,
                                 nsACString* aStdout, int* aExitStatus);
  static bool ResolveDesktopId(nsACString& aDesktopId);
  static bool IsPlasmaSession();
  static bool HasPlasmaWallpaperTool();
  static nsresult WriteElementImageToPng(mozilla::dom::Element* aElement,
                                         const nsACString& aPath);
  bool DesktopIdMatchesHandler(const nsACString& aHandlerId) const;
};

#endif
