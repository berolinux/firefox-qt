/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_MenuUtilsQt_h
#define mozilla_widget_MenuUtilsQt_h

class QAction;

namespace mozilla {
namespace dom {
class Element;
}
namespace widget {

// Apply XUL key/modifiers to a QAction (for dbusmenu-qt and native popups).
void ApplyMenuShortcuts(QAction* aAction, const dom::Element* aElement);

// Best-effort icon from NativeMenu::GetIcon (file: and theme names only).
void ApplyMenuIcon(QAction* aAction, dom::Element* aElement);

}  // namespace widget
}  // namespace mozilla

#endif
