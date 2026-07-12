/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsUnixDesktopService_h_
#define nsUnixDesktopService_h_

#include "nsIGIOService.h"

// Same contract/CID as legacy GIO so preferences / exthandler keep working.
#define NS_UNIXDESKTOPSERVICE_CID \
  {0xe3a1f3c9, 0x3ae1, 0x4b40, {0xa5, 0xe0, 0x7b, 0x45, 0x7f, 0xc9, 0xa9, 0xad}}

/**
 * FreeDesktop / Qt desktop integration for protocol and MIME handlers.
 *
 * Replaces GIO on the Qt toolkit: xdg-mime / xdg-open / desktop files /
 * mimeapps.list, KDE helpers (kreadconfig, kioclient) when present, Qt
 * QMimeDatabase + QDesktopServices, and optional KF6Service.
 */
class nsUnixDesktopService final : public nsIGIOService {
  ~nsUnixDesktopService() = default;

 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIGIOSERVICE
};

#endif
