/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FdoNotificationBridge.h"

#include "mozilla/Logging.h"

#pragma GCC visibility push(default)
#include <QDBusConnection>
#include <QDBusInterface>
#pragma GCC visibility pop

using namespace mozilla;

static LazyLogModule gQtAlertsLog("QtAlerts");
#define LOG(...) \
  MOZ_LOG(gQtAlertsLog, LogLevel::Debug, (__VA_ARGS__))

bool FdoNotificationBridge::Connect() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    LOG("No session bus");
    return false;
  }

  QDBusInterface iface(QStringLiteral("org.freedesktop.Notifications"),
                       QStringLiteral("/org/freedesktop/Notifications"),
                       QStringLiteral("org.freedesktop.Notifications"), bus);
  if (!iface.isValid()) {
    LOG("org.freedesktop.Notifications not available: %s",
        qPrintable(iface.lastError().message()));
    return false;
  }

  // Connect signals only once — EnsureConnected may retry after early failure.
  if (!mSignalsConnected) {
    bus.connect(QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("/org/freedesktop/Notifications"),
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("NotificationClosed"), this,
                SLOT(NotificationClosed(uint, uint)));
    bus.connect(QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("/org/freedesktop/Notifications"),
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("ActionInvoked"), this,
                SLOT(ActionInvoked(uint, QString)));
    mSignalsConnected = true;
  }
  return true;
}

void FdoNotificationBridge::NotificationClosed(uint aId, uint aReason) {
  if (mService) {
    mService->OnNotificationClosed(aId, aReason);
  }
}

void FdoNotificationBridge::ActionInvoked(uint aId, const QString& aAction) {
  if (mService) {
    mService->OnActionInvoked(
        aId, nsDependentCString(aAction.toUtf8().constData()));
  }
}
