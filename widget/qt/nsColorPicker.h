/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_nsColorPicker_h
#define widget_qt_nsColorPicker_h

#include "nsBaseColorPicker.h"

class nsColorPicker final : public nsBaseColorPicker {
 public:
  nsColorPicker() = default;
  NS_DECL_ISUPPORTS

 private:
  ~nsColorPicker() = default;
  nsresult InitNative(const nsTArray<nsString>& aDefaultColors) override;
  nsresult OpenNative() override;
};

#endif
