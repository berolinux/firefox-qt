/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_GeoclueLocationBridge_h
#define mozilla_widget_GeoclueLocationBridge_h

#include "GeoclueLocationProviderQt.h"

#pragma GCC visibility push(default)
#include <QObject>
#include <QString>
#include <QtDBus/QDBusObjectPath>
#pragma GCC visibility pop

namespace mozilla::widget {

class GeoclueLocationBridge final : public QObject {
  Q_OBJECT
 public:
  explicit GeoclueLocationBridge(GeoclueLocationProviderQt* aOwner)
      : mOwner(aOwner) {}
  void Clear() { mOwner = nullptr; }
  void SetClientPath(const QString& aPath) { mClientPath = aPath; }
  QString ClientPath() const { return mClientPath; }

 public Q_SLOTS:
  void OnLocationUpdated(const QDBusObjectPath& aOld,
                         const QDBusObjectPath& aNew);

 private:
  GeoclueLocationProviderQt* mOwner = nullptr;
  QString mClientPath;
};

}  // namespace mozilla::widget

#endif
