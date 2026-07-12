/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_a11y_AccessibleWrap_h_
#define mozilla_a11y_AccessibleWrap_h_

#include "LocalAccessible.h"

namespace mozilla {
namespace a11y {

class QtAccessible;

/**
 * Qt QAccessible-backed wrap for local accessibles.
 * Native interface is a QtAccessible* (QAccessibleInterface).
 */
class AccessibleWrap : public LocalAccessible {
 public:
  AccessibleWrap(nsIContent* aContent, DocAccessible* aDoc);
  virtual ~AccessibleWrap();

  virtual void Shutdown() override;
  virtual void GetNativeInterface(void** aOutAccessible) override;

  QtAccessible* GetQtAccessible() const { return mQtAccessible; }

  // Adopt an interface created by the QAccessible factory (not yet registered
  // by us — Qt will register it).
  void AdoptQtAccessible(QtAccessible* aIface);

  void ShutdownQtAccessible();

 private:
  QtAccessible* mQtAccessible = nullptr;
};

}  // namespace a11y
}  // namespace mozilla

#endif
