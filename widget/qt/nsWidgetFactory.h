/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_nsWidgetFactory_h
#define widget_qt_nsWidgetFactory_h

#include "nscore.h"
#include "nsID.h"

nsresult nsAppShellConstructor(const nsIID& iid, void** result);

nsresult nsWidgetQtModuleCtor();
void nsWidgetQtModuleDtor();

#endif
