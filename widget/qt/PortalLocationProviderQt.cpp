/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PortalLocationProviderQt.h"
#include "PortalLocationBridge.h"

#include "GeolocationPosition.h"
#include "MLSFallback.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Logging.h"
#include "mozilla/XREAppData.h"
#include "mozilla/dom/GeolocationPositionErrorBinding.h"
#include "nsThreadUtils.h"
#include "prtime.h"

#pragma GCC visibility push(default)
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusObjectPath>
#include <QtDBus/QDBusPendingReply>
#pragma GCC visibility pop

extern const mozilla::XREAppData* gAppData;

namespace mozilla::widget {

static LazyLogModule sQtPortalLocLog("PortalLocationQt");
#define LOG(...) \
  MOZ_LOG(sQtPortalLocLog, LogLevel::Debug, (__VA_ARGS__))

void PortalLocationBridge::OnLocationUpdated(const QDBusObjectPath& aSession,
                                             const QVariantMap& aData) {
  if (!mOwner) {
    return;
  }
  if (!aData.contains(QStringLiteral("Latitude")) ||
      !aData.contains(QStringLiteral("Longitude"))) {
    return;
  }
  const double lat = aData.value(QStringLiteral("Latitude")).toDouble();
  const double lon = aData.value(QStringLiteral("Longitude")).toDouble();
  double alt = UnspecifiedNaN<double>();
  if (aData.contains(QStringLiteral("Altitude"))) {
    alt = aData.value(QStringLiteral("Altitude")).toDouble();
  }
  double accuracy = UnspecifiedNaN<double>();
  if (aData.contains(QStringLiteral("Accuracy"))) {
    accuracy = aData.value(QStringLiteral("Accuracy")).toDouble();
  }
  double heading = UnspecifiedNaN<double>();
  if (aData.contains(QStringLiteral("Heading"))) {
    heading = aData.value(QStringLiteral("Heading")).toDouble();
  }
  double speed = UnspecifiedNaN<double>();
  if (aData.contains(QStringLiteral("Speed"))) {
    speed = aData.value(QStringLiteral("Speed")).toDouble();
  }
  (void)aSession;
  mOwner->OnLocationUpdated(lat, lon, alt, accuracy, heading, speed);
}

NS_IMPL_ISUPPORTS(PortalLocationProviderQt, nsIGeolocationProvider,
                  nsITimerCallback, nsINamed)

PortalLocationProviderQt::PortalLocationProviderQt() = default;

PortalLocationProviderQt::~PortalLocationProviderQt() {
  if (mBridge || mRefreshTimer || mMLSProvider) {
    Shutdown();
  }
}

void PortalLocationProviderQt::SetRefreshTimer(int aDelayMs) {
  if (!mRefreshTimer) {
    mRefreshTimer = NS_NewTimer();
  }
  if (mRefreshTimer) {
    mRefreshTimer->InitWithCallback(this, aDelayMs, nsITimer::TYPE_ONE_SHOT);
  }
}

NS_IMETHODIMP
PortalLocationProviderQt::Notify(nsITimer*) {
  if (mCallback && mLastPosition) {
    mCallback->Update(mLastPosition);
    SetRefreshTimer(5000);
  }
  return NS_OK;
}

NS_IMETHODIMP
PortalLocationProviderQt::GetName(nsACString& aName) {
  aName.AssignLiteral("PortalLocationProviderQt");
  return NS_OK;
}

void PortalLocationProviderQt::OnLocationUpdated(double aLat, double aLon,
                                                 double aAlt, double aAccuracy,
                                                 double aHeading,
                                                 double aSpeed) {
  if (!mCallback) {
    return;
  }
  if (mMLSProvider) {
    mMLSProvider->Shutdown(MLSFallback::ShutdownReason::ProviderResponded);
    mMLSProvider = nullptr;
  }
  mLastPosition = new nsGeoPosition(aLat, aLon, aAlt, aAccuracy, 0.0, aHeading,
                                    aSpeed, PR_Now() / PR_USEC_PER_MSEC);
  mCallback->Update(mLastPosition);
  SetRefreshTimer(5000);
}

void PortalLocationProviderQt::NotifyError(uint16_t aError) {
  if (!mCallback) {
    return;
  }
  if (!mMLSProvider) {
    mMLSProvider = MakeAndAddRef<MLSFallback>(0);
    mMLSProvider->Startup(mCallback);
  }
  mCallback->NotifyError(aError);
}

NS_IMETHODIMP
PortalLocationProviderQt::Startup() {
  LOG("Startup portal location (QtDBus)");
  if (!QDBusConnection::sessionBus().isConnected()) {
    return NS_OK;
  }

  auto* bridge = new PortalLocationBridge(this);
  mBridge = bridge;

  QDBusConnection::sessionBus().connect(
      QStringLiteral("org.freedesktop.portal.Desktop"),
      QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.Location"),
      QStringLiteral("LocationUpdated"), bridge,
      SLOT(OnLocationUpdated(QDBusObjectPath,QVariantMap)));

  nsAutoCString token("firefox_location");
  if (gAppData) {
    gAppData->GetDBusAppName(token);
  }
  QVariantMap sessionOpts;
  sessionOpts.insert(QStringLiteral("session_handle_token"),
                     QString::fromUtf8(token.get()));

  QDBusMessage createMsg = QDBusMessage::createMethodCall(
      QStringLiteral("org.freedesktop.portal.Desktop"),
      QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.Location"),
      QStringLiteral("CreateSession"));
  createMsg << sessionOpts;

  QDBusPendingReply<QDBusObjectPath> createReply =
      QDBusConnection::sessionBus().asyncCall(createMsg, 10000);
  createReply.waitForFinished();
  if (createReply.isError()) {
    LOG("CreateSession failed: %s",
        qPrintable(createReply.error().message()));
    return NS_OK;
  }
  mSessionPath.Assign(createReply.value().path().toUtf8().constData());

  QVariantMap startOpts;
  QDBusMessage startMsg = QDBusMessage::createMethodCall(
      QStringLiteral("org.freedesktop.portal.Desktop"),
      QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.Location"),
      QStringLiteral("Start"));
  startMsg << QString::fromUtf8(mSessionPath.get()) << QString() << startOpts;
  QDBusConnection::sessionBus().asyncCall(startMsg, 30000);
  LOG("Location Start issued for session %s", mSessionPath.get());
  return NS_OK;
}

NS_IMETHODIMP
PortalLocationProviderQt::Watch(nsIGeolocationUpdate* aCallback) {
  mCallback = aCallback;
  if (!mMLSProvider) {
    mMLSProvider = MakeAndAddRef<MLSFallback>(2000);
    mMLSProvider->Startup(aCallback);
  }
  return NS_OK;
}

NS_IMETHODIMP
PortalLocationProviderQt::Shutdown() {
  LOG("Shutdown");
  if (mRefreshTimer) {
    mRefreshTimer->Cancel();
    mRefreshTimer = nullptr;
  }
  if (mMLSProvider) {
    mMLSProvider->Shutdown(MLSFallback::ShutdownReason::ProviderShutdown);
    mMLSProvider = nullptr;
  }
  mCallback = nullptr;
  mLastPosition = nullptr;
  if (mBridge) {
    auto* bridge = static_cast<PortalLocationBridge*>(mBridge);
    bridge->Clear();
    QDBusConnection::sessionBus().disconnect(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Location"),
        QStringLiteral("LocationUpdated"), bridge,
        SLOT(OnLocationUpdated(QDBusObjectPath,QVariantMap)));
    bridge->deleteLater();
    mBridge = nullptr;
  }
  if (!mSessionPath.IsEmpty()) {
    QDBusMessage closeMsg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QString::fromUtf8(mSessionPath.get()),
        QStringLiteral("org.freedesktop.portal.Session"),
        QStringLiteral("Close"));
    QDBusConnection::sessionBus().asyncCall(closeMsg);
    mSessionPath.Truncate();
  }
  return NS_OK;
}

NS_IMETHODIMP
PortalLocationProviderQt::SetHighAccuracy(bool) { return NS_OK; }

}  // namespace mozilla::widget
