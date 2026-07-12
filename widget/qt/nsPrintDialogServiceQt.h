/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsPrintDialogServiceQt_h_
#define nsPrintDialogServiceQt_h_

#include "nsIPrintDialogService.h"

class nsPrintDialogServiceQt final : public nsIPrintDialogService {
  ~nsPrintDialogServiceQt() = default;

 public:
  nsPrintDialogServiceQt() = default;

  NS_DECL_ISUPPORTS
  NS_DECL_NSIPRINTDIALOGSERVICE
};

#endif
