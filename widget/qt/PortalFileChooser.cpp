/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PortalFileChooser.h"

#include "mozilla/Logging.h"
#include "nsIFilePicker.h"
#include "nsString.h"

#include <unistd.h>

#pragma GCC visibility push(default)
#include <QUrl>
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusMetaType>
#include <QtDBus/QDBusObjectPath>
#include <QtDBus/QDBusPendingReply>
#include <QtDBus/QDBusVariant>
#pragma GCC visibility pop

namespace mozilla::widget {

static LazyLogModule gPortalFileChooserLog("QtFilePicker");
#define LOG(...) \
  MOZ_LOG(gPortalFileChooserLog, LogLevel::Debug, (__VA_ARGS__))

// Portal FileChooser filters: a(sa(us))
// (filter_name, array of (glob_type=0 for glob, pattern)).
// Use named structs (not QPair) so qDBusRegisterMetaType + stream ops work.
struct PortalGlob {
  uint mType = 0;
  QString mPattern;
};
struct PortalFilter {
  QString mName;
  QList<PortalGlob> mGlobs;
};

static QString ToQ(const nsAString& aStr) {
  return QString::fromUtf16(aStr.BeginReading(), int(aStr.Length()));
}

static QDBusArgument& operator<<(QDBusArgument& aArg, const PortalGlob& aGlob) {
  aArg.beginStructure();
  aArg << aGlob.mType << aGlob.mPattern;
  aArg.endStructure();
  return aArg;
}
static const QDBusArgument& operator>>(const QDBusArgument& aArg,
                                       PortalGlob& aGlob) {
  aArg.beginStructure();
  aArg >> aGlob.mType >> aGlob.mPattern;
  aArg.endStructure();
  return aArg;
}

static QDBusArgument& operator<<(QDBusArgument& aArg,
                                 const PortalFilter& aFilter) {
  aArg.beginStructure();
  aArg << aFilter.mName << aFilter.mGlobs;
  aArg.endStructure();
  return aArg;
}
static const QDBusArgument& operator>>(const QDBusArgument& aArg,
                                       PortalFilter& aFilter) {
  aArg.beginStructure();
  aArg >> aFilter.mName >> aFilter.mGlobs;
  aArg.endStructure();
  return aArg;
}

static void RegisterPortalFilterMetaTypes() {
  static bool sRegistered = false;
  if (sRegistered) {
    return;
  }
  sRegistered = true;
  qDBusRegisterMetaType<PortalGlob>();
  qDBusRegisterMetaType<QList<PortalGlob>>();
  qDBusRegisterMetaType<PortalFilter>();
  qDBusRegisterMetaType<QList<PortalFilter>>();
}

static QList<PortalFilter> BuildPortalFilters(
    const nsTArray<nsString>& aFilters,
    const nsTArray<nsString>& aFilterNames) {
  QList<PortalFilter> list;
  for (size_t i = 0; i < aFilters.Length(); ++i) {
    PortalFilter filter;
    filter.mName =
        (i < aFilterNames.Length() && !aFilterNames[i].IsEmpty())
            ? ToQ(aFilterNames[i])
            : ToQ(aFilters[i]);
    nsCString remaining = NS_ConvertUTF16toUTF8(aFilters[i]);
    while (!remaining.IsEmpty()) {
      int32_t sep = remaining.FindChar(';');
      if (sep < 0) {
        sep = remaining.FindChar(' ');
      }
      nsAutoCString piece;
      if (sep < 0) {
        piece = remaining;
        remaining.Truncate();
      } else {
        piece.Assign(Substring(remaining, 0, sep));
        remaining.Cut(0, sep + 1);
      }
      piece.Trim(" \t");
      if (piece.IsEmpty() || piece.EqualsLiteral("*.*") ||
          piece.EqualsLiteral("*")) {
        continue;
      }
      filter.mGlobs.append(PortalGlob{0u, QString::fromUtf8(piece.get())});
    }
    if (filter.mGlobs.isEmpty()) {
      continue;
    }
    list.append(std::move(filter));
  }
  return list;
}

PortalFileChooser::PortalFileChooser(DoneCallback aDone)
    : mDone(std::move(aDone)) {}

PortalFileChooser::~PortalFileChooser() { DisconnectResponse(); }

/* static */
QString PortalFileChooser::MakeHandleToken() {
  static uint32_t sSerial = 0;
  // Portal tokens must be ASCII [A-Za-z0-9_].
  return QStringLiteral("firefox_file_picker_%1_%2")
      .arg(++sSerial)
      .arg(uint32_t(getpid()));
}

/* static */
QString PortalFileChooser::PortalRequestPath(const QString& aToken) {
  QString sender = QDBusConnection::sessionBus().baseService();  // :1.42
  if (sender.startsWith(QLatin1Char(':'))) {
    sender = sender.mid(1);
  }
  sender.replace(QLatin1Char('.'), QLatin1Char('_'));
  return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
      .arg(sender, aToken);
}

void PortalFileChooser::DisconnectResponse() {
  if (mRequestPath.isEmpty()) {
    return;
  }
  QDBusConnection::sessionBus().disconnect(
      QStringLiteral("org.freedesktop.portal.Desktop"), mRequestPath,
      QStringLiteral("org.freedesktop.portal.Request"),
      QStringLiteral("Response"), this,
      SLOT(OnResponse(uint,QVariantMap)));
  mRequestPath.clear();
}

void PortalFileChooser::Finish(int32_t aResultCode,
                               const QStringList& aPaths) {
  if (mFinished) {
    return;
  }
  mFinished = true;
  DisconnectResponse();
  if (mDone) {
    mDone(aResultCode, aPaths);
  }
  deleteLater();
}

void PortalFileChooser::OnResponse(uint aResponse,
                                   const QVariantMap& aResults) {
  LOG("portal Response code=%u", aResponse);
  if (aResponse != 0) {
    Finish(int32_t(nsIFilePicker::returnCancel), {});
    return;
  }
  const QVariant urisVar = aResults.value(QStringLiteral("uris"));
  QStringList uris = urisVar.toStringList();
  if (uris.isEmpty() && urisVar.canConvert<QDBusArgument>()) {
    const QDBusArgument arg = urisVar.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
      QString u;
      arg >> u;
      uris << u;
    }
    arg.endArray();
  }

  QStringList paths;
  for (const QString& uri : uris) {
    const QUrl url(uri);
    if (url.isLocalFile()) {
      paths << url.toLocalFile();
    } else if (uri.startsWith(QLatin1Char('/'))) {
      paths << uri;
    }
  }
  Finish(paths.isEmpty() ? int32_t(nsIFilePicker::returnCancel)
                         : int32_t(nsIFilePicker::returnOK),
         paths);
}

void PortalFileChooser::OnOpenFinished(QDBusPendingCallWatcher* aWatcher) {
  QDBusPendingReply<QDBusObjectPath> reply = *aWatcher;
  aWatcher->deleteLater();
  if (reply.isError()) {
    LOG("portal Open/Save call failed: %s",
        qPrintable(reply.error().message()));
    Finish(int32_t(nsIFilePicker::returnCancel), {});
    return;
  }
  // Prefer the object path returned by the portal.
  const QString path = reply.value().path();
  if (!path.isEmpty() && path != mRequestPath) {
    DisconnectResponse();
    mRequestPath = path;
    if (!QDBusConnection::sessionBus().connect(
            QStringLiteral("org.freedesktop.portal.Desktop"), mRequestPath,
            QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Response"), this,
            SLOT(OnResponse(uint, QVariantMap)))) {
      LOG("failed to connect Response on %s", qPrintable(mRequestPath));
      Finish(int32_t(nsIFilePicker::returnCancel), {});
    }
  }
}

bool PortalFileChooser::Start(int32_t aMode, const nsAString& aTitle,
                              const nsAString& aDefault,
                              const nsTArray<nsString>& aFilters,
                              const nsTArray<nsString>& aFilterNames) {
  if (!QDBusConnection::sessionBus().isConnected()) {
    return false;
  }

  RegisterPortalFilterMetaTypes();

  const QString token = MakeHandleToken();
  mRequestPath = PortalRequestPath(token);

  // Subscribe before the method call so we cannot miss a fast Response.
  if (!QDBusConnection::sessionBus().connect(
          QStringLiteral("org.freedesktop.portal.Desktop"), mRequestPath,
          QStringLiteral("org.freedesktop.portal.Request"),
          QStringLiteral("Response"), this,
          SLOT(OnResponse(uint, QVariantMap)))) {
    LOG("failed to pre-connect Response on %s", qPrintable(mRequestPath));
    return false;
  }

  // Options are a{sv}: every value must be a D-Bus VARIANT. Plain bools and
  // strings are fine; complex types need QDBusVariant wrapping. Putting a raw
  // QDBusArgument in the map crashes in QDBusMarshaller::appendVariantInternal
  // (Save Link As).
  QVariantMap options;
  options.insert(QStringLiteral("handle_token"), token);
  options.insert(QStringLiteral("multiple"),
                 aMode == nsIFilePicker::modeOpenMultiple);
  options.insert(QStringLiteral("directory"),
                 aMode == nsIFilePicker::modeGetFolder);
  options.insert(QStringLiteral("modal"), true);
  if (!aDefault.IsEmpty() && aMode == nsIFilePicker::modeSave) {
    // Basename only — full paths belong in current_folder / current_file.
    QString name = ToQ(aDefault);
    const int slash = std::max(name.lastIndexOf(QLatin1Char('/')),
                               name.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0) {
      name = name.mid(slash + 1);
    }
    if (!name.isEmpty()) {
      options.insert(QStringLiteral("current_name"), name);
    }
  }

  QList<PortalFilter> filters = BuildPortalFilters(aFilters, aFilterNames);
  if (!filters.isEmpty()) {
    // a(sa(us)) via registered meta-types, wrapped as VARIANT for a{sv}.
    options.insert(
        QStringLiteral("filters"),
        QVariant::fromValue(QDBusVariant(QVariant::fromValue(filters))));
  }

  const char* method =
      aMode == nsIFilePicker::modeSave ? "SaveFile" : "OpenFile";
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QStringLiteral("org.freedesktop.portal.Desktop"),
      QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.FileChooser"),
      QString::fromLatin1(method));
  msg << QString()  // parent window handle (optional)
      << ToQ(aTitle) << options;

  QDBusPendingCall pending =
      QDBusConnection::sessionBus().asyncCall(msg, 120000);
  auto* watcher = new QDBusPendingCallWatcher(pending, this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          &PortalFileChooser::OnOpenFinished);
  return true;
}

}  // namespace mozilla::widget
