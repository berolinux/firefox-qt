/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GeoclueLocationProviderQt.h"
#include "GeoclueLocationBridge.h"

#include "GeolocationPosition.h"
#include "MLSFallback.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Logging.h"
#include "mozilla/StaticPrefs_geo.h"
#include "mozilla/XREAppData.h"
#include "mozilla/dom/GeolocationPositionErrorBinding.h"
#include "nsThreadUtils.h"
#include "prtime.h"

#include <cmath>

#pragma GCC visibility push(default)
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusObjectPath>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#include <QtDBus/QDBusReply>
#include <QtDBus/QDBusVariant>
#pragma GCC visibility pop

extern const mozilla::XREAppData* gAppData;

namespace mozilla::widget {

static LazyLogModule sGeoclueQtLog("GeoclueLocationQt");
#define LOG(...) \
  MOZ_LOG(sGeoclueQtLog, LogLevel::Debug, (__VA_ARGS__))

namespace {

constexpr const char kBusName[] = "org.freedesktop.GeoClue2";
constexpr const char kManagerPath[] = "/org/freedesktop/GeoClue2/Manager";
constexpr const char kManagerIface[] = "org.freedesktop.GeoClue2.Manager";
constexpr const char kClientIface[] = "org.freedesktop.GeoClue2.Client";
constexpr const char kLocationIface[] = "org.freedesktop.GeoClue2.Location";
constexpr const char kPropsIface[] = "org.freedesktop.DBus.Properties";

// Matches GClueAccuracyLevel.
enum class GCAccuracyLevel : uint32_t {
  City = 4,
  Exact = 8,
};

QVariant UnwrapVariant(QVariant v) {
  while (v.userType() == qMetaTypeId<QDBusVariant>()) {
    v = v.value<QDBusVariant>().variant();
  }
  return v;
}

bool SetClientProperty(const QString& aClientPath, const char* aName,
                       const QVariant& aValue) {
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kBusName), aClientPath,
      QString::fromLatin1(kPropsIface), QStringLiteral("Set"));
  msg << QString::fromLatin1(kClientIface) << QString::fromLatin1(aName)
      << QVariant::fromValue(QDBusVariant(aValue));
  QDBusMessage reply =
      QDBusConnection::systemBus().call(msg, QDBus::Block, 5000);
  if (reply.type() == QDBusMessage::ErrorMessage) {
    LOG("Set %s failed: %s", aName, qPrintable(reply.errorMessage()));
    return false;
  }
  return true;
}

}  // namespace

void GeoclueLocationBridge::OnLocationUpdated(const QDBusObjectPath& aOld,
                                              const QDBusObjectPath& aNew) {
  (void)aOld;
  if (!mOwner || aNew.path().isEmpty()) {
    return;
  }
  mOwner->OnLocationPath(
      nsDependentCString(aNew.path().toUtf8().constData()));
}

NS_IMPL_ISUPPORTS(GeoclueLocationProviderQt, nsIGeolocationProvider,
                  nsITimerCallback, nsINamed)

GeoclueLocationProviderQt::GeoclueLocationProviderQt() {
  mHighAccuracy = StaticPrefs::geo_provider_geoclue_always_high_accuracy();
}

GeoclueLocationProviderQt::~GeoclueLocationProviderQt() {
  if (mBridge || mRefreshTimer || mMLSProvider || mStarted) {
    Shutdown();
  }
}

void GeoclueLocationProviderQt::SetRefreshTimer(int aDelayMs) {
  if (!mRefreshTimer) {
    mRefreshTimer = NS_NewTimer();
  }
  if (mRefreshTimer) {
    mRefreshTimer->InitWithCallback(this, aDelayMs, nsITimer::TYPE_ONE_SHOT);
  }
}

NS_IMETHODIMP
GeoclueLocationProviderQt::Notify(nsITimer*) {
  if (mCallback && mLastPosition) {
    mCallback->Update(mLastPosition);
    SetRefreshTimer(5000);
  }
  return NS_OK;
}

NS_IMETHODIMP
GeoclueLocationProviderQt::GetName(nsACString& aName) {
  aName.AssignLiteral("GeoclueLocationProviderQt");
  return NS_OK;
}

void GeoclueLocationProviderQt::NotifyError(uint16_t aError) {
  if (!mCallback) {
    return;
  }
  if (!mMLSProvider) {
    mMLSProvider = MakeAndAddRef<MLSFallback>(0);
    mMLSProvider->Startup(mCallback);
  }
  mCallback->NotifyError(aError);
}

void GeoclueLocationProviderQt::OnDBusError(const nsACString& aMessage) {
  LOG("D-Bus error: %s", PromiseFlatCString(aMessage).get());
  NotifyError(dom::GeolocationPositionError_Binding::POSITION_UNAVAILABLE);
}

void GeoclueLocationProviderQt::OnLocationData(double aLat, double aLon,
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

void GeoclueLocationProviderQt::RequestLocationProps(const nsACString& aPath) {
  const nsCString path(aPath);
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kBusName),
      QString::fromUtf8(path.get(), qsizetype(path.Length())),
      QString::fromLatin1(kPropsIface), QStringLiteral("GetAll"));
  msg << QString::fromLatin1(kLocationIface);
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::systemBus().asyncCall(msg, 5000));
  RefPtr<GeoclueLocationProviderQt> self = this;
  QObject::connect(
      watcher, &QDBusPendingCallWatcher::finished,
      [self](QDBusPendingCallWatcher* w) {
        QDBusPendingReply<QVariantMap> reply = *w;
        w->deleteLater();
        if (reply.isError()) {
          self->OnDBusError(
              nsDependentCString(reply.error().message().toUtf8().constData()));
          return;
        }
        QVariantMap props = reply.value();
        auto getDouble = [&](const char* key, double fallback) {
          if (!props.contains(QString::fromLatin1(key))) {
            return fallback;
          }
          return UnwrapVariant(props.value(QString::fromLatin1(key))).toDouble();
        };
        const double lat = getDouble("Latitude", UnspecifiedNaN<double>());
        const double lon = getDouble("Longitude", UnspecifiedNaN<double>());
        if (std::isnan(lat) || std::isnan(lon)) {
          return;
        }
        double alt = getDouble("Altitude", UnspecifiedNaN<double>());
        // GeoClue may report bogus altitudes below sea level extremes.
        if (!std::isnan(alt) && alt < -500.0) {
          alt = UnspecifiedNaN<double>();
        }
        const double accuracy =
            getDouble("Accuracy", UnspecifiedNaN<double>());
        const double heading = getDouble("Heading", UnspecifiedNaN<double>());
        const double speed = getDouble("Speed", UnspecifiedNaN<double>());
        self->OnLocationData(lat, lon, alt, accuracy, heading, speed);
      });
}

void GeoclueLocationProviderQt::OnLocationPath(const nsACString& aPath) {
  LOG("Location path: %s", PromiseFlatCString(aPath).get());
  RequestLocationProps(aPath);
}

void GeoclueLocationProviderQt::SetDesktopIdAndAccuracy() {
  if (mClientPath.IsEmpty()) {
    return;
  }
  const QString client = QString::fromUtf8(mClientPath.get());

  nsAutoCString appName("firefox");
  if (gAppData) {
    gAppData->GetDBusAppName(appName);
  }
  if (!SetClientProperty(client, "DesktopId",
                         QString::fromUtf8(appName.get()))) {
    OnDBusError("Failed to set DesktopId"_ns);
    return;
  }

  const uint32_t level = mHighAccuracy
                             ? uint32_t(GCAccuracyLevel::Exact)
                             : uint32_t(GCAccuracyLevel::City);
  if (!SetClientProperty(client, "RequestedAccuracyLevel", level)) {
    OnDBusError("Failed to set RequestedAccuracyLevel"_ns);
    return;
  }

  StartClient();
}

void GeoclueLocationProviderQt::StartClient() {
  if (mClientPath.IsEmpty()) {
    return;
  }
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kBusName), QString::fromUtf8(mClientPath.get()),
      QString::fromLatin1(kClientIface), QStringLiteral("Start"));
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::systemBus().asyncCall(msg, 10000));
  RefPtr<GeoclueLocationProviderQt> self = this;
  QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                   [self](QDBusPendingCallWatcher* w) {
                     QDBusPendingReply<> reply = *w;
                     w->deleteLater();
                     if (reply.isError()) {
                       self->OnDBusError(nsDependentCString(
                           reply.error().message().toUtf8().constData()));
                       return;
                     }
                     self->mStarted = true;
                     LOG("Client started");
                   });
}

void GeoclueLocationProviderQt::StopClient() {
  if (mClientPath.IsEmpty() || !mStarted) {
    mStarted = false;
    return;
  }
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kBusName), QString::fromUtf8(mClientPath.get()),
      QString::fromLatin1(kClientIface), QStringLiteral("Stop"));
  QDBusConnection::systemBus().asyncCall(msg, 5000);
  mStarted = false;
}

NS_IMETHODIMP
GeoclueLocationProviderQt::Startup() {
  LOG("Startup GeoClue (QtDBus)");
  QDBusConnection sys = QDBusConnection::systemBus();
  if (!sys.isConnected()) {
    LOG("system bus not connected");
    return NS_ERROR_FAILURE;
  }
  // Confirm GeoClue manager is available (sync, short timeout).
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kBusName), QString::fromLatin1(kManagerPath),
      QString::fromLatin1(kPropsIface), QStringLiteral("Get"));
  msg << QString::fromLatin1(kManagerIface)
      << QStringLiteral("InUse");
  QDBusMessage reply = sys.call(msg, QDBus::Block, 3000);
  if (reply.type() == QDBusMessage::ErrorMessage) {
    LOG("GeoClue Manager not available: %s",
        qPrintable(reply.errorMessage()));
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

NS_IMETHODIMP
GeoclueLocationProviderQt::Watch(nsIGeolocationUpdate* aCallback) {
  mCallback = aCallback;
  if (!mMLSProvider) {
    mMLSProvider = MakeAndAddRef<MLSFallback>(2000);
    mMLSProvider->Startup(aCallback);
  }

  if (mStarted && !mClientPath.IsEmpty()) {
    return NS_OK;
  }

  if (!mBridge) {
    mBridge = new GeoclueLocationBridge(this);
  }

  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kBusName), QString::fromLatin1(kManagerPath),
      QString::fromLatin1(kManagerIface), QStringLiteral("GetClient"));
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::systemBus().asyncCall(msg, 5000));
  RefPtr<GeoclueLocationProviderQt> self = this;
  QObject::connect(
      watcher, &QDBusPendingCallWatcher::finished,
      [self](QDBusPendingCallWatcher* w) {
        QDBusPendingReply<QDBusObjectPath> reply = *w;
        w->deleteLater();
        if (reply.isError()) {
          self->OnDBusError(
              nsDependentCString(reply.error().message().toUtf8().constData()));
          return;
        }
        const QString path = reply.value().path();
        if (path.isEmpty()) {
          self->OnDBusError("Empty GeoClue client path"_ns);
          return;
        }
        self->mClientPath.Assign(path.toUtf8().constData());
        LOG("Client path: %s", self->mClientPath.get());

        auto* bridge = static_cast<GeoclueLocationBridge*>(self->mBridge);
        if (bridge) {
          if (!bridge->ClientPath().isEmpty()) {
            QDBusConnection::systemBus().disconnect(
                QString::fromLatin1(kBusName), bridge->ClientPath(),
                QString::fromLatin1(kClientIface),
                QStringLiteral("LocationUpdated"), bridge,
                SLOT(OnLocationUpdated(QDBusObjectPath,QDBusObjectPath)));
          }
          bridge->SetClientPath(path);
          QDBusConnection::systemBus().connect(
              QString::fromLatin1(kBusName), path,
              QString::fromLatin1(kClientIface),
              QStringLiteral("LocationUpdated"), bridge,
              SLOT(OnLocationUpdated(QDBusObjectPath,QDBusObjectPath)));
        }
        self->SetDesktopIdAndAccuracy();
      });
  return NS_OK;
}

NS_IMETHODIMP
GeoclueLocationProviderQt::Shutdown() {
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
  StopClient();
  if (mBridge) {
    auto* bridge = static_cast<GeoclueLocationBridge*>(mBridge);
    if (!bridge->ClientPath().isEmpty()) {
      QDBusConnection::systemBus().disconnect(
          QString::fromLatin1(kBusName), bridge->ClientPath(),
          QString::fromLatin1(kClientIface), QStringLiteral("LocationUpdated"),
          bridge, SLOT(OnLocationUpdated(QDBusObjectPath,QDBusObjectPath)));
    }
    bridge->Clear();
    bridge->deleteLater();
    mBridge = nullptr;
  }
  mClientPath.Truncate();
  return NS_OK;
}

NS_IMETHODIMP
GeoclueLocationProviderQt::SetHighAccuracy(bool aHigh) {
  if (!aHigh && StaticPrefs::geo_provider_geoclue_always_high_accuracy()) {
    aHigh = true;
  }
  if (mHighAccuracy == aHigh) {
    return NS_OK;
  }
  mHighAccuracy = aHigh;
  if (!mClientPath.IsEmpty()) {
    // GeoClue requires restart to apply a new accuracy level.
    StopClient();
    SetDesktopIdAndAccuracy();
  }
  return NS_OK;
}

}  // namespace mozilla::widget
