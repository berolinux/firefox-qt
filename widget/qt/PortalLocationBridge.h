/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_PortalLocationBridge_h
#define mozilla_widget_PortalLocationBridge_h

#include "PortalLocationProviderQt.h"

#pragma GCC visibility push(default)
#include <QObject>
#include <QVariantMap>
#include <QtDBus/QDBusObjectPath>
#pragma GCC visibility pop

namespace mozilla::widget {

class PortalLocationBridge final : public QObject {
  Q_OBJECT
 public:
  explicit PortalLocationBridge(PortalLocationProviderQt* aOwner)
      : mOwner(aOwner) {}
  void Clear() { mOwner = nullptr; }

 public Q_SLOTS:
  void OnLocationUpdated(const QDBusObjectPath& aSession,
                         const QVariantMap& aData);

 private:
  PortalLocationProviderQt* mOwner;
};

}  // namespace mozilla::widget

#endif
