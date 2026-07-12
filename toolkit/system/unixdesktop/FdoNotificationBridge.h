/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef FdoNotificationBridge_h_
#define FdoNotificationBridge_h_

#include "nsSystemAlertsServiceQt.h"

#pragma GCC visibility push(default)
#include <QObject>
#include <QString>
#pragma GCC visibility pop

// QDBus signal receiver for org.freedesktop.Notifications.
class FdoNotificationBridge final : public QObject {
  Q_OBJECT
 public:
  explicit FdoNotificationBridge(nsSystemAlertsServiceQt* aService)
      : mService(aService) {}

  bool Connect();

 public Q_SLOTS:
  void NotificationClosed(uint aId, uint aReason);
  void ActionInvoked(uint aId, const QString& aAction);

 private:
  nsSystemAlertsServiceQt* mService;
  bool mSignalsConnected = false;
};

#endif
