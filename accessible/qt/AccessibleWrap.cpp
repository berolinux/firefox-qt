/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AccessibleWrap.h"
#include "QtAccessible.h"

#include "LocalAccessible-inl.h"

#pragma GCC visibility push(default)
#include <QAccessible>
#pragma GCC visibility pop

using namespace mozilla;
using namespace mozilla::a11y;

AccessibleWrap::AccessibleWrap(nsIContent* aContent, DocAccessible* aDoc)
    : LocalAccessible(aContent, aDoc) {}

AccessibleWrap::~AccessibleWrap() { MOZ_ASSERT(!mQtAccessible); }

void AccessibleWrap::Shutdown() {
  ShutdownQtAccessible();
  LocalAccessible::Shutdown();
}

void AccessibleWrap::ShutdownQtAccessible() {
  if (!mQtAccessible) {
    return;
  }
  mQtAccessible->ClearAcc();
  const QAccessible::Id id = QAccessible::uniqueId(mQtAccessible);
  if (id) {
    QAccessible::deleteAccessibleInterface(id);
  }
  mQtAccessible = nullptr;
}

void AccessibleWrap::AdoptQtAccessible(QtAccessible* aIface) {
  MOZ_ASSERT(aIface);
  MOZ_ASSERT(!mQtAccessible || mQtAccessible == aIface);
  mQtAccessible = aIface;
}

void AccessibleWrap::GetNativeInterface(void** aOutAccessible) {
  *aOutAccessible = nullptr;
  if (IsDefunct()) {
    return;
  }

  if (!mQtAccessible) {
    QObject* obj = nullptr;
    if (IsRoot()) {
      if (QWindow* win = QtAccessible::WindowFor(this)) {
        obj = win;
        // May invoke the factory, which adopts into this wrap.
        (void)QAccessible::queryAccessibleInterface(obj);
      }
    }
    if (!mQtAccessible) {
      mQtAccessible = new QtAccessible(this, obj);
      QAccessible::registerAccessibleInterface(mQtAccessible);
    }
  }
  *aOutAccessible = mQtAccessible;
}
