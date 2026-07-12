/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_NativeMenuQt_h
#define mozilla_widget_NativeMenuQt_h

#include "mozilla/EventForwards.h"
#include "mozilla/widget/NativeMenu.h"
#include "mozilla/UniquePtr.h"

class QMenu;

namespace mozilla {

namespace dom {
class Element;
}

namespace widget {

class NativeMenuQt final : public NativeMenu {
 public:
  static bool CanUse();

  explicit NativeMenuQt(dom::Element* aPopup);
  ~NativeMenuQt() override;

  void ShowMenuAnchored(nsIFrame* aClickedFrame,
                        const nsMenuPopupFrame* aPopupFrame) override;
  void ShowMenuAtPosition(nsIFrame* aClickedFrame,
                          const CSSIntPoint& aPosition,
                          bool aIsContextMenu) override;
  bool Close() override;
  void ActivateItem(dom::Element* aItemElement, Modifiers aModifiers,
                    int16_t aButton, ErrorResult& aRv) override;
  void OpenSubmenu(dom::Element* aMenuElement) override;
  void CloseSubmenu(dom::Element* aMenuElement) override;
  RefPtr<dom::Element> Element() override;
  void AddObserver(NativeMenu::Observer* aObserver) override {
    mObservers.AppendElement(aObserver);
  }
  void RemoveObserver(NativeMenu::Observer* aObserver) override {
    mObservers.RemoveElement(aObserver);
  }

  // Called from QAction::triggered (same translation unit / friend PopulateMenu).
  void OnItemTriggered(dom::Element* aItem);

 private:
  void Rebuild();
  void FinishHideAndActivate();
  void OnAboutToHide();
  void FirePopupEvent(EventMessage aMessage);

  RefPtr<dom::Element> mElement;
  // Set when a QAction is chosen; command runs in FinishHideAndActivate
  // after QMenu has fully hidden (avoids re-entrancy crash) but *before*
  // popuphidden so gContextMenu is still valid.
  RefPtr<dom::Element> mPendingActivation;
  mozilla::UniquePtr<QMenu> mMenu;
  bool mShowing = false;
  bool mHideCompletionQueued = false;
  nsTArray<NativeMenu::Observer*> mObservers;
};

}  // namespace widget
}  // namespace mozilla

#endif
