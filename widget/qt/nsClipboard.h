/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_nsClipboard_h
#define widget_qt_nsClipboard_h

#include "nsBaseClipboard.h"

class nsClipboard final : public nsBaseClipboard {
 public:
  nsClipboard();

  NS_DECL_ISUPPORTS_INHERITED

 protected:
  ~nsClipboard() = default;

  NS_IMETHOD SetNativeClipboardData(nsITransferable* aTransferable,
                                    ClipboardType aWhichClipboard) override;
  mozilla::Result<nsCOMPtr<nsISupports>, nsresult> GetNativeClipboardData(
      const nsACString& aFlavor, ClipboardType aWhichClipboard,
      uint64_t aThreshold = 0) override;
  nsresult EmptyNativeClipboardData(ClipboardType aWhichClipboard) override;
  mozilla::Result<int32_t, nsresult> GetNativeClipboardSequenceNumber(
      ClipboardType aWhichClipboard) override;
  mozilla::Result<bool, nsresult> HasNativeClipboardDataMatchingFlavors(
      const nsTArray<nsCString>& aFlavorList,
      ClipboardType aWhichClipboard) override;
};

#endif
