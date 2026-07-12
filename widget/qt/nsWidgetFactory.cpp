/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsWidgetFactory.h"

#include "mozilla/Components.h"
#include "mozilla/WidgetUtils.h"
#include "mozilla/widget/ScreenManager.h"
#include "nsAppShell.h"
#include "nsAppShellSingleton.h"
#include "nsWidgetsCID.h"
#include "nsLookAndFeel.h"
#include "nsWindow.h"
#include "nsClipboard.h"
#include "HeadlessClipboard.h"
#include "gfxPlatform.h"

using namespace mozilla;
using namespace mozilla::widget;

NS_IMPL_COMPONENT_FACTORY(nsIClipboard) {
  nsCOMPtr<nsIClipboard> inst;
  if (gfxPlatform::IsHeadless()) {
    inst = new HeadlessClipboard();
  } else {
    inst = new nsClipboard();
  }
  return inst.forget().downcast<nsISupports>();
}

nsresult nsWidgetQtModuleCtor() { return nsAppShellInit(); }

void nsWidgetQtModuleDtor() {
  WidgetUtils::Shutdown();
  nsLookAndFeel::Shutdown();
  nsWindow::ReleaseGlobals();
  nsAppShellShutdown();
}
