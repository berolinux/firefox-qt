/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/widget/NativeMenuSupport.h"

#include "DBusMenuBar.h"
#include "MainThreadUtils.h"
#include "NativeMenuQt.h"
#include "QtPrefUtils.h"
#include "nsWindow.h"

namespace mozilla::widget {

void NativeMenuSupport::CreateNativeMenuBar(nsIWidget* aParent,
                                            dom::Element* aMenuBarElement) {
  MOZ_RELEASE_ASSERT(NS_IsMainThread(),
                     "Attempting to create native menu bar on wrong thread!");

#ifdef MOZ_ENABLE_DBUS
  if (aMenuBarElement && PrefGlobalMenuEnabled() && DBusMenuBar::Available()) {
    if (auto* window = nsWindow::FromWidget(aParent)) {
      if (RefPtr bar = DBusMenuBar::Create(aMenuBarElement)) {
        window->SetDBusMenuBar(std::move(bar));
      }
    }
  }
#else
  (void)aParent;
  (void)aMenuBarElement;
#endif
}

already_AddRefed<NativeMenu> NativeMenuSupport::CreateNativePopupMenu(
    dom::Element* aPopup) {
  if (!NativeMenuQt::CanUse() || !aPopup) {
    return nullptr;
  }
  return MakeAndAddRef<NativeMenuQt>(aPopup);
}

bool NativeMenuSupport::ShouldUseNativeAnchoredMenus() { return false; }

bool NativeMenuSupport::ShouldUseNativeContextMenus() {
  return NativeMenuQt::CanUse();
}

}  // namespace mozilla::widget
