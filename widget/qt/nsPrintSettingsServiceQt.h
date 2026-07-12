/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsPrintSettingsServiceQt_h
#define nsPrintSettingsServiceQt_h

#include "nsPrintSettingsService.h"
#include "nsIPrintSettings.h"

class nsPrintSettingsServiceQt final : public nsPrintSettingsService {
 public:
  nsPrintSettingsServiceQt() {}

  nsresult _CreatePrintSettings(nsIPrintSettings** _retval) override;
};

#endif  // nsPrintSettingsServiceQt_h
