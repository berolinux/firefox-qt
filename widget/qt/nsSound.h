/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_nsSound_h
#define widget_qt_nsSound_h

#include "nsISound.h"
#include "nsCOMPtr.h"

class nsSound final : public nsISound {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSISOUND

  static already_AddRefed<nsISound> GetInstance();

 private:
  nsSound() = default;
  ~nsSound() = default;
};

#endif
