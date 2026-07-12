/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsColorPicker.h"
#include "nsIColorPicker.h"

#include <QColorDialog>
#include <QColor>

NS_IMPL_ISUPPORTS(nsColorPicker, nsIColorPicker)

nsresult nsColorPicker::InitNative(const nsTArray<nsString>&) { return NS_OK; }

nsresult nsColorPicker::OpenNative() {
  QColor initial = QColorDialog::getColor();
  // Async callback is handled by base after OpenNative in real implementations;
  // for now, complete immediately via mCallback if set by Open().
  if (mCallback) {
    if (initial.isValid()) {
      nsAutoString color;
      color.AssignLiteral("#");
      nsAutoString hex;
      hex.AppendInt(initial.red(), 16);
      if (hex.Length() == 1) color.Append('0');
      color.Append(hex);
      hex.Truncate();
      hex.AppendInt(initial.green(), 16);
      if (hex.Length() == 1) color.Append('0');
      color.Append(hex);
      hex.Truncate();
      hex.AppendInt(initial.blue(), 16);
      if (hex.Length() == 1) color.Append('0');
      color.Append(hex);
      mCallback->Update(color);
      mCallback->Done(color);
    } else {
      mCallback->Done(nsString());
    }
  }
  return NS_OK;
}
