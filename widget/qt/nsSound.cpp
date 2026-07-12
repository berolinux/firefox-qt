/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsSound.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ClearOnShutdown.h"
#include "gfxPlatform.h"
#include "HeadlessSound.h"

#pragma GCC visibility push(default)
#include <QApplication>
#pragma GCC visibility pop

using namespace mozilla;

NS_IMPL_ISUPPORTS(nsSound, nsISound)

static StaticRefPtr<nsISound> sSoundInstance;

already_AddRefed<nsISound> nsSound::GetInstance() {
  if (!sSoundInstance) {
    if (gfxPlatform::IsHeadless()) {
      sSoundInstance = new widget::HeadlessSound();
    } else {
      sSoundInstance = new nsSound();
    }
    ClearOnShutdown(&sSoundInstance);
  }
  return do_AddRef(sSoundInstance);
}

NS_IMETHODIMP nsSound::Beep() {
  QApplication::beep();
  return NS_OK;
}

NS_IMETHODIMP nsSound::Init() { return NS_OK; }

NS_IMETHODIMP nsSound::PlayEventSound(uint32_t) {
  // Optional system event sounds; platform theme may handle some of these.
  return NS_OK;
}
