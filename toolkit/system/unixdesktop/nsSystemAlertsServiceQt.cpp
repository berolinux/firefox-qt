/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsSystemAlertsServiceQt.h"
#include "FdoNotificationBridge.h"
#include "mozilla/gfx/2D.h"

#include "imgIContainer.h"
#include "mozilla/Logging.h"
#include "mozilla/XREAppData.h"
#include "nsIObserver.h"
#include "nsIStringBundle.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "nsPrintfCString.h"
#include "prenv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#pragma GCC visibility push(default)
#include <QBuffer>
#include <QByteArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusError>
#include <QGuiApplication>
#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#pragma GCC visibility pop

using namespace mozilla;

extern const XREAppData* gAppData;

static LazyLogModule gQtAlertsLog("QtAlerts");
#define LOG(...) \
  MOZ_LOG(gQtAlertsLog, LogLevel::Debug, (__VA_ARGS__))

// ---------------------------------------------------------------------------
// DBus image-data helper (iiibiiay)
// ---------------------------------------------------------------------------

struct FdoImageData {
  int width = 0;
  int height = 0;
  int rowstride = 0;
  bool hasAlpha = true;
  int bitsPerSample = 8;
  int channels = 4;
  QByteArray data;
};

QDBusArgument& operator<<(QDBusArgument& aArg, const FdoImageData& aImage) {
  aArg.beginStructure();
  aArg << aImage.width << aImage.height << aImage.rowstride << aImage.hasAlpha
       << aImage.bitsPerSample << aImage.channels << aImage.data;
  aArg.endStructure();
  return aArg;
}

const QDBusArgument& operator>>(const QDBusArgument& aArg, FdoImageData& aImage) {
  aArg.beginStructure();
  aArg >> aImage.width >> aImage.height >> aImage.rowstride >> aImage.hasAlpha >>
      aImage.bitsPerSample >> aImage.channels >> aImage.data;
  aArg.endStructure();
  return aArg;
}

Q_DECLARE_METATYPE(FdoImageData)

static bool ImageToFdoData(imgIContainer* aImage, FdoImageData& aOut) {
  if (!aImage) {
    return false;
  }
  // Never FLAG_SYNC_DECODE here: that blocks the main thread (and with
  // D-Bus BlockWithGui re-entrancy) freezes the content process UI.
  RefPtr<gfx::SourceSurface> surface =
      aImage->GetFrame(imgIContainer::FRAME_CURRENT, imgIContainer::FLAG_NONE);
  if (!surface) {
    return false;
  }
  RefPtr<gfx::DataSourceSurface> data = surface->GetDataSurface();
  if (!data) {
    return false;
  }
  gfx::DataSourceSurface::ScopedMap map(data, gfx::DataSourceSurface::READ);
  if (!map.IsMapped()) {
    return false;
  }

  const int width = data->GetSize().width;
  const int height = data->GetSize().height;
  if (width <= 0 || height <= 0 || width > 512 || height > 512) {
    return false;
  }

  // FreeDesktop wants unpremultiplied RGBA, 8 bpc.
  aOut.width = width;
  aOut.height = height;
  aOut.rowstride = width * 4;
  aOut.hasAlpha = true;
  aOut.bitsPerSample = 8;
  aOut.channels = 4;
  aOut.data.resize(width * height * 4);

  const auto fmt = data->GetFormat();
  const uint8_t* src = map.GetData();
  const int32_t srcStride = map.GetStride();
  char* dst = aOut.data.data();

  for (int y = 0; y < height; ++y) {
    const uint8_t* row = src + y * srcStride;
    for (int x = 0; x < width; ++x) {
      uint8_t r = 0, g = 0, b = 0, a = 255;
      switch (fmt) {
        case gfx::SurfaceFormat::B8G8R8A8:
        case gfx::SurfaceFormat::B8G8R8X8:
          b = row[x * 4 + 0];
          g = row[x * 4 + 1];
          r = row[x * 4 + 2];
          a = row[x * 4 + 3];
          break;
        case gfx::SurfaceFormat::R8G8B8A8:
        case gfx::SurfaceFormat::R8G8B8X8:
          r = row[x * 4 + 0];
          g = row[x * 4 + 1];
          b = row[x * 4 + 2];
          a = row[x * 4 + 3];
          break;
        default:
          b = row[x * 4 + 0];
          g = row[x * 4 + 1];
          r = row[x * 4 + 2];
          a = row[x * 4 + 3];
          break;
      }
      // Unpremultiply if needed for FDO.
      if (a > 0 && a < 255) {
        r = uint8_t(std::min(255, (int(r) * 255 + a / 2) / a));
        g = uint8_t(std::min(255, (int(g) * 255 + a / 2) / a));
        b = uint8_t(std::min(255, (int(b) * 255 + a / 2) / a));
      }
      dst[0] = char(r);
      dst[1] = char(g);
      dst[2] = char(b);
      dst[3] = char(a);
      dst += 4;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// nsSystemAlertsServiceQt
// ---------------------------------------------------------------------------

NS_IMPL_ISUPPORTS(nsSystemAlertsServiceQt, nsIAlertsService,
                  nsIAlertsDoNotDisturb)

nsSystemAlertsServiceQt::nsSystemAlertsServiceQt() = default;

nsSystemAlertsServiceQt::~nsSystemAlertsServiceQt() {
  if (mDBusBridge) {
    delete static_cast<FdoNotificationBridge*>(mDBusBridge);
    mDBusBridge = nullptr;
  }
}

nsresult nsSystemAlertsServiceQt::Init() {
  qDBusRegisterMetaType<FdoImageData>();
  auto* bridge = new FdoNotificationBridge(this);
  mDBusBridge = bridge;
  mConnected = bridge->Connect();
  if (!mConnected) {
    LOG("Init: FreeDesktop Notifications unavailable");
  }
  return NS_OK;  // Prefer soft init; ShowAlert fails clearly if no bus.
}

/* static */
nsCString nsSystemAlertsServiceQt::GetAppName() {
  nsCOMPtr<nsIStringBundleService> bundleService =
      do_GetService(NS_STRINGBUNDLE_CONTRACTID);
  if (bundleService) {
    nsCOMPtr<nsIStringBundle> bundle;
    bundleService->CreateBundle("chrome://branding/locale/brand.properties",
                                getter_AddRefs(bundle));
    if (bundle) {
      nsAutoString appName;
      if (NS_SUCCEEDED(bundle->GetStringFromName("brandShortName", appName)) &&
          !appName.IsEmpty()) {
        return NS_ConvertUTF16toUTF8(appName);
      }
    }
  }
  return "Mozilla"_ns;
}

/* static */
nsCString nsSystemAlertsServiceQt::GetDesktopEntry() {
  if (const char* env = PR_GetEnv("MOZ_DESKTOP_FILE_NAME")) {
    nsCString entry(env);
    if (StringEndsWith(entry, ".desktop"_ns)) {
      entry.Truncate(entry.Length() - strlen(".desktop"));
    }
    return entry;
  }
  if (gAppData && gAppData->remotingName) {
    return nsCString(gAppData->remotingName);
  }
  return "firefox"_ns;
}

bool nsSystemAlertsServiceQt::EnsureConnected() {
  if (mConnected) {
    return true;
  }
  if (!mDBusBridge) {
    mDBusBridge = new FdoNotificationBridge(this);
  }
  mConnected = static_cast<FdoNotificationBridge*>(mDBusBridge)->Connect();
  return mConnected;
}

nsresult nsSystemAlertsServiceQt::ShowFreedesktopNotification(
    nsIAlertNotification* aAlert, nsIObserver* aListener,
    const nsAString& aName) {
  if (!EnsureConnected()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  bool textClickable = false;
  (void)aAlert->GetTextClickable(&textClickable);

  nsTArray<RefPtr<nsIAlertAction>> actions;
  (void)aAlert->GetActions(actions);

  bool silent = false;
  (void)aAlert->GetSilent(&silent);

  bool requireInteraction = false;
  (void)aAlert->GetRequireInteraction(&requireInteraction);

  nsAutoString title;
  MOZ_TRY(aAlert->GetTitle(title));
  if (title.IsEmpty()) {
    title.AssignLiteral(" ");
  }

  nsAutoString text;
  MOZ_TRY(aAlert->GetText(text));

  nsAutoString cookie;
  MOZ_TRY(aAlert->GetCookie(cookie));

  // replaces_id for same-name alerts (FDO tag replace). Defer alertfinished
  // until Notify succeeds so a failed replace does not drop both.
  uint32_t replacesId = 0;
  PendingAlert replaced;
  bool hadReplaced = false;
  if (auto nameEntry = mNameToId.Lookup(aName)) {
    replacesId = *nameEntry;
    nameEntry.Remove();
    if (auto pending = mPending.Lookup(replacesId)) {
      replaced = std::move(*pending);
      hadReplaced = true;
      pending.Remove();
    }
  }

  QStringList fdoActions;
  if (textClickable) {
    fdoActions << QStringLiteral("default") << QStringLiteral("Activate");
  }
  // Cap actions: some servers misbehave with large lists; FDO pairs of strings.
  constexpr size_t kMaxFdoActions = 4;
  size_t actionCount = 0;
  for (const auto& action : actions) {
    if (!action || actionCount >= kMaxFdoActions) {
      break;
    }
    nsAutoString actionName, actionTitle;
    if (NS_FAILED(action->GetAction(actionName)) ||
        NS_FAILED(action->GetTitle(actionTitle))) {
      continue;
    }
    nsAutoCString actionKey;
    AppendUTF16toUTF8(actionName, actionKey);
    actionKey.AppendLiteral("-moz");
    fdoActions << QString::fromUtf8(actionKey.get())
               << QString::fromUtf16(
                      reinterpret_cast<const char16_t*>(actionTitle.BeginReading()),
                      qsizetype(actionTitle.Length()));
    ++actionCount;
  }

  QVariantMap hints;
  hints.insert(QStringLiteral("desktop-entry"),
               QString::fromUtf8(GetDesktopEntry().get()));
  hints.insert(QStringLiteral("suppress-sound"), silent);
  hints.insert(QStringLiteral("urgency"), QVariant::fromValue(uchar(1)));

  // Optional image only if already decoded (never sync-decode on this thread).
  nsCOMPtr<imgIContainer> image;
  if (NS_SUCCEEDED(aAlert->GetImage(getter_AddRefs(image))) && image) {
    FdoImageData imgData;
    if (ImageToFdoData(image, imgData)) {
      hints.insert(QStringLiteral("image-data"), QVariant::fromValue(imgData));
    }
  }

  // Match GTK (nsAlertsIconListener): only force never-expire when
  // requireInteraction is set; otherwise leave server default (-1), same as
  // libnotify NOTIFY_EXPIRES_DEFAULT when set_timeout is not called.
  constexpr int kNotifyExpiresNever = 0;
  constexpr int kNotifyExpiresDefault = -1;
  const int expireTimeout =
      requireInteraction ? kNotifyExpiresNever : kNotifyExpiresDefault;

  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    NS_WARNING("QtAlerts: no D-Bus session bus");
    if (hadReplaced) {
      mPending.InsertOrUpdate(replacesId, std::move(replaced));
      mNameToId.InsertOrUpdate(aName, replacesId);
    }
    return NS_ERROR_NOT_AVAILABLE;
  }

  auto toQ = [](const nsAString& s) {
    return QString::fromUtf16(
        reinterpret_cast<const char16_t*>(s.BeginReading()),
        qsizetype(s.Length()));
  };

  QDBusMessage msg = QDBusMessage::createMethodCall(
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("/org/freedesktop/Notifications"),
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("Notify"));
  // Signature: susssasa{sv}i — do NOT use BlockWithGui (re-enters the event
  // loop, freezes the browser, and drops subsequent notifications).
  msg << QString::fromUtf8(GetAppName().get()) << uint(replacesId)
      << QStringLiteral("firefox") << toQ(title) << toQ(text) << fdoActions
      << hints << expireTimeout;

  QDBusMessage response = bus.call(msg, QDBus::Block, /* timeout ms */ 2000);
  QDBusReply<uint> reply(response);
  if (!reply.isValid()) {
    // One cheap retry without image-data / extra actions.
    QVariantMap simpleHints;
    simpleHints.insert(QStringLiteral("desktop-entry"),
                       QString::fromUtf8(GetDesktopEntry().get()));
    simpleHints.insert(QStringLiteral("urgency"),
                       QVariant::fromValue(uchar(1)));
    QDBusMessage msg2 = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("Notify"));
    msg2 << QString::fromUtf8(GetAppName().get()) << uint(replacesId)
         << QStringLiteral("firefox") << toQ(title) << toQ(text)
         << QStringList() << simpleHints << expireTimeout;
    response = bus.call(msg2, QDBus::Block, 2000);
    reply = QDBusReply<uint>(response);
  }

  if (!reply.isValid()) {
    NS_WARNING(nsPrintfCString("QtAlerts: Notify failed: %s",
                               reply.error().message().toUtf8().constData())
                   .get());
    if (hadReplaced) {
      mPending.InsertOrUpdate(replacesId, std::move(replaced));
      mNameToId.InsertOrUpdate(aName, replacesId);
    }
    return NS_ERROR_FAILURE;
  }

  const uint32_t id = reply.value();
  LOG("Notify ok id=%u name=%s", id, NS_ConvertUTF16toUTF8(aName).get());

  if (hadReplaced) {
    if (nsCOMPtr<nsIObserver> listener = replaced.mListener) {
      listener->Observe(nullptr, "alertfinished", replaced.mCookie.get());
    }
  }

  PendingAlert pending;
  pending.mListener = aListener;
  pending.mCookie = cookie;
  pending.mName = aName;
  pending.mNotification = aAlert;
  mPending.InsertOrUpdate(id, std::move(pending));
  mNameToId.InsertOrUpdate(aName, id);

  if (aListener) {
    aListener->Observe(nullptr, "alertshow", cookie.get());
  }
  return NS_OK;
}

namespace {

// nsAlertsService calls the system backend with nsIAlertCallbacks. This
// backend still notifies through the older observer topics.
class AlertCallbacksToObserver final : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS

  explicit AlertCallbacksToObserver(nsIAlertCallbacks* aCallbacks)
      : mCallbacks(aCallbacks) {}

  NS_IMETHOD Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* /* aData */) override {
    if (!mCallbacks || !aTopic) {
      return NS_OK;
    }
    if (!strcmp(aTopic, "alertshow")) {
      return mCallbacks->OnAlertShow();
    }
    if (!strcmp(aTopic, "alertclickcallback")) {
      nsCOMPtr<nsIAlertAction> action = do_QueryInterface(aSubject);
      return mCallbacks->OnAlertClick(action);
    }
    if (!strcmp(aTopic, "alertfinished")) {
      return mCallbacks->OnAlertFinished();
    }
    if (!strcmp(aTopic, "alertsettingscallback")) {
      return mCallbacks->OnAlertSettings();
    }
    if (!strcmp(aTopic, "alertdisablecallback")) {
      return mCallbacks->OnAlertDisable();
    }
    return NS_OK;
  }

 private:
  virtual ~AlertCallbacksToObserver() = default;

  nsCOMPtr<nsIAlertCallbacks> mCallbacks;
};

NS_IMPL_ISUPPORTS(AlertCallbacksToObserver, nsIObserver)

}  // namespace

NS_IMETHODIMP
nsSystemAlertsServiceQt::ShowAlert(nsIAlertNotification* aAlert,
                                   nsIObserver* aAlertListener) {
  NS_ENSURE_ARG(aAlert);

  if (mSuppressForScreenSharing || mDoNotDisturb) {
    nsAutoString cookie;
    if (NS_SUCCEEDED(aAlert->GetCookie(cookie)) && aAlertListener) {
      aAlertListener->Observe(nullptr, "alertfinished", cookie.get());
    }
    return NS_OK;
  }

  nsAutoString name;
  MOZ_TRY(aAlert->GetName(name));
  return ShowFreedesktopNotification(aAlert, aAlertListener, name);
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::ShowAlertWithCallbacks(
    nsIAlertNotification* aAlert, nsIAlertCallbacks* aAlertCallbacks) {
  nsCOMPtr<nsIObserver> observer;
  if (aAlertCallbacks) {
    observer = new AlertCallbacksToObserver(aAlertCallbacks);
  }
  return ShowAlert(aAlert, observer);
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::CloseAlert(const nsAString& aAlertName,
                                    bool /*aContextClosed*/) {
  auto nameEntry = mNameToId.Lookup(aAlertName);
  if (!nameEntry) {
    return NS_OK;
  }
  const uint32_t id = *nameEntry;
  nameEntry.Remove();

  if (auto pending = mPending.Lookup(id)) {
    if (nsCOMPtr<nsIObserver> listener = pending->mListener) {
      listener->Observe(nullptr, "alertfinished", pending->mCookie.get());
    }
    pending.Remove();
  }

  QDBusInterface iface(
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("/org/freedesktop/Notifications"),
      QStringLiteral("org.freedesktop.Notifications"),
      QDBusConnection::sessionBus());
  if (iface.isValid()) {
    iface.call(QStringLiteral("CloseNotification"), uint(id));
  }
  return NS_OK;
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::GetHistory(nsTArray<nsString>& aResult) {
  // FDO Notifications has no history API; empty list is fine for cleanup.
  aResult.Clear();
  return NS_OK;
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::GetManualDoNotDisturb(bool* aRetVal) {
  NS_ENSURE_ARG_POINTER(aRetVal);
  *aRetVal = mDoNotDisturb;
  return NS_OK;
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::SetManualDoNotDisturb(bool aDoNotDisturb) {
  mDoNotDisturb = aDoNotDisturb;
  return NS_OK;
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::IsFullscreen(bool* aRetVal) {
  *aRetVal = false;
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::GetSuppressForScreenSharing(bool* aRetVal) {
  NS_ENSURE_ARG_POINTER(aRetVal);
  *aRetVal = mSuppressForScreenSharing;
  return NS_OK;
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::SetSuppressForScreenSharing(bool aSuppress) {
  mSuppressForScreenSharing = aSuppress;
  return NS_OK;
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::Teardown() {
  // Close all outstanding notifications.
  nsTArray<nsString> names;
  for (auto iter = mNameToId.Iter(); !iter.Done(); iter.Next()) {
    names.AppendElement(iter.Key());
  }
  for (const auto& name : names) {
    (void)CloseAlert(name, false);
  }
  mPending.Clear();
  mNameToId.Clear();
  return NS_OK;
}

NS_IMETHODIMP
nsSystemAlertsServiceQt::PbmTeardown() {
  // FDO notifications are not partitioned by private browsing; close all
  // outstanding alerts (same as Teardown).
  return Teardown();
}

void nsSystemAlertsServiceQt::OnNotificationClosed(uint32_t aId,
                                                   uint32_t /*aReason*/) {
  auto pending = mPending.Lookup(aId);
  if (!pending) {
    return;
  }
  const nsString name = pending->mName;
  if (nsCOMPtr<nsIObserver> listener = pending->mListener) {
    listener->Observe(nullptr, "alertfinished", pending->mCookie.get());
  }
  pending.Remove();
  mNameToId.Remove(name);
}

void nsSystemAlertsServiceQt::OnActionInvoked(uint32_t aId,
                                              const nsACString& aActionKey) {
  auto pending = mPending.Lookup(aId);
  if (!pending || !pending->mListener) {
    return;
  }

  nsCString key(aActionKey);
  if (key.EqualsLiteral("default")) {
    pending->mListener->Observe(nullptr, "alertclickcallback",
                                pending->mCookie.get());
    return;
  }

  // Strip "-moz" suffix for custom actions.
  if (StringEndsWith(key, "-moz"_ns)) {
    key.Truncate(key.Length() - 4);
  }
  nsCOMPtr<nsIAlertAction> alertAction;
  if (pending->mNotification) {
    pending->mNotification->GetAction(NS_ConvertUTF8toUTF16(key),
                                      getter_AddRefs(alertAction));
  }
  pending->mListener->Observe(alertAction, "alertclickcallback",
                              pending->mCookie.get());
}
