/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsPrintSettingsServiceQt.h"

#include "nsPrintSettingsImpl.h"

// PDF is the intermediate format we hand to CUPS (same as GTK).
// Do not force a dummy printer name; the frontend / CUPS list supply that.
class nsPrintSettingsQt final : public nsPrintSettings {
 public:
  nsPrintSettingsQt() {
    SetOutputFormat(nsIPrintSettings::kOutputFormatPDF);
  }
};

nsresult nsPrintSettingsServiceQt::_CreatePrintSettings(
    nsIPrintSettings** _retval) {
  nsPrintSettings* printSettings = new nsPrintSettingsQt();
  NS_ENSURE_TRUE(printSettings, NS_ERROR_OUT_OF_MEMORY);
  NS_ADDREF(*_retval = printSettings);
  (void)InitPrintSettingsFromPrefs(*_retval, false,
                                   nsIPrintSettings::kInitSaveAll);
  return NS_OK;
}

already_AddRefed<nsIPrintSettings> CreatePlatformPrintSettings(
    const mozilla::PrintSettingsInitializer& aSettings) {
  RefPtr<nsPrintSettings> settings = new nsPrintSettingsQt();
  settings->InitWithInitializer(aSettings);
  return settings.forget();
}
