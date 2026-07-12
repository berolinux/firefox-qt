/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsUserIdleServiceQt.h"

#include "mozilla/TimeStamp.h"

using namespace mozilla;

static TimeStamp sLastActivity = TimeStamp::Now();

// static
void nsUserIdleServiceQt::NotifyUserActivity() {
  sLastActivity = TimeStamp::Now();
}

bool nsUserIdleServiceQt::PollIdleTime(uint32_t* aIdleTime) {
  if (!aIdleTime) {
    return false;
  }
  // System-wide idle (XScreenSaver / Mutter) can be layered on later.
  // For now, report time since the last input event handled by a Qt window.
  if (sLastActivity.IsNull()) {
    *aIdleTime = 0;
    return true;
  }
  *aIdleTime = uint32_t((TimeStamp::Now() - sLastActivity).ToMilliseconds());
  return true;
}
