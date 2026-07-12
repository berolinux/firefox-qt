/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "NativeMessagingPortalBackend.h"

#include "jsapi.h"
#include "js/PropertyAndElement.h"
#include "mozilla/Logging.h"
#include "mozilla/XREAppData.h"
#include "mozilla/dom/ScriptSettings.h"
#include "nsThreadUtils.h"

#include <fcntl.h>
#include <unistd.h>

#pragma GCC visibility push(default)
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusError>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusMetaType>
#include <QtDBus/QDBusPendingReply>
#include <QtDBus/QDBusUnixFileDescriptor>
#include <QtDBus/QDBusVariant>
#pragma GCC visibility pop

static mozilla::LazyLogModule gNMPBackendLog("NativeMessagingPortal");
#define LOG_NMP(...) \
  MOZ_LOG(gNMPBackendLog, mozilla::LogLevel::Debug, (__VA_ARGS__))

namespace mozilla::extensions {

namespace {

constexpr const char kPortalService[] = "org.freedesktop.portal.Desktop";
constexpr const char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr const char kWebExtIface[] = "org.freedesktop.portal.WebExtensions";
constexpr const char kSessionIface[] = "org.freedesktop.portal.Session";
constexpr const char kRequestIface[] = "org.freedesktop.portal.Request";

void MakePortalRequestToken(const nsACString& aType, nsACString& aToken) {
  static Atomic<unsigned, MemoryOrdering::Relaxed> sSerial;
  aToken.Truncate();
  aToken.AppendPrintf("%s_%u_%u", PromiseFlatCString(aType).get(),
                      unsigned(sSerial++), unsigned(getpid()));
  XREAppData::SanitizeNameForDBus(aToken);
}

QString PortalRequestPath(const QString& aToken) {
  QString sender = QDBusConnection::sessionBus().baseService();
  if (sender.startsWith(QLatin1Char(':'))) {
    sender = sender.mid(1);
  }
  sender.replace(QLatin1Char('.'), QLatin1Char('_'));
  return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
      .arg(sender, aToken);
}

void RejectOpError(dom::Promise& aPromise, const QString& aMsg) {
  aPromise.MaybeRejectWithOperationError(
      nsDependentCString(aMsg.toUtf8().constData()));
}

}  // namespace

NativeMessagingPortalBackend::NativeMessagingPortalBackend(
    NativeMessagingPortal* aOwner)
    : mOwner(aOwner) {
  qRegisterMetaType<QDBusUnixFileDescriptor>("QDBusUnixFileDescriptor");
  qDBusRegisterMetaType<QDBusUnixFileDescriptor>();
}

void NativeMessagingPortalBackend::ProbeAvailability() {
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
      QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
  msg << QString::fromLatin1(kWebExtIface) << QStringLiteral("version");
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(msg, 5000), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          &NativeMessagingPortalBackend::OnVersionFinished);
}

void NativeMessagingPortalBackend::OnVersionFinished(
    QDBusPendingCallWatcher* aWatcher) {
  QDBusPendingReply<QDBusVariant> reply = *aWatcher;
  aWatcher->deleteLater();
  bool available = false;
  if (!reply.isError()) {
    QVariant v = reply.value().variant();
    while (v.userType() == qMetaTypeId<QDBusVariant>()) {
      v = v.value<QDBusVariant>().variant();
    }
    bool ok = false;
    available = v.toUInt(&ok) >= 1 && ok;
  } else {
    // Interface missing or error — try ping via introspect-less method call
    // CreateSession would fail later; mark unavailable.
    LOG_NMP("version Get failed: %s", qPrintable(reply.error().message()));
  }
  // Also consider the name owner present as available for older portals.
  if (!available) {
    available = QDBusConnection::sessionBus().interface()->isServiceRegistered(
        QString::fromLatin1(kPortalService));
    // Only if WebExtensions iface might exist — optimistic for flatpak.
  }
  if (mOwner) {
    mOwner->mAvailable = available;
    mOwner->mReady = true;
    mOwner->DrainPending();
  }
  LOG_NMP("portal available=%d", available);
}

void NativeMessagingPortalBackend::CreateSession(
    const nsCString& aToken, const RefPtr<dom::Promise>& aPromise) {
  QVariantMap options;
  options.insert(QStringLiteral("session_handle_token"),
                 QString::fromUtf8(aToken.get()));
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
      QString::fromLatin1(kWebExtIface), QStringLiteral("CreateSession"));
  msg << options;
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(msg, 15000), this);
  RefPtr<dom::Promise> promise = aPromise;
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, promise](QDBusPendingCallWatcher* w) {
            OnCreateSessionFinished(w, promise);
          });
}

void NativeMessagingPortalBackend::OnCreateSessionFinished(
    QDBusPendingCallWatcher* aWatcher, RefPtr<dom::Promise> aPromise) {
  QDBusPendingReply<QDBusObjectPath> reply = *aWatcher;
  aWatcher->deleteLater();
  if (reply.isError()) {
    RejectOpError(*aPromise, reply.error().message());
    return;
  }
  const QString path = reply.value().path();
  LOG_NMP("session created %s", qPrintable(path));
  if (mOwner) {
    mOwner->mSessions[path.toStdString()] = SessionState::Active;
  }
  SubscribeSessionClosed(path);
  aPromise->MaybeResolve(nsDependentCString(path.toUtf8().constData()));
}

void NativeMessagingPortalBackend::CloseSession(
    const nsCString& aHandle, const RefPtr<dom::Promise>& aPromise) {
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortalService), QString::fromUtf8(aHandle.get()),
      QString::fromLatin1(kSessionIface), QStringLiteral("Close"));
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(msg, 10000), this);
  RefPtr<dom::Promise> promise = aPromise;
  nsCString handle = aHandle;
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, promise, handle](QDBusPendingCallWatcher* w) {
            OnCloseSessionFinished(w, promise, handle);
          });
}

void NativeMessagingPortalBackend::OnCloseSessionFinished(
    QDBusPendingCallWatcher* aWatcher, RefPtr<dom::Promise> aPromise,
    nsCString aHandle) {
  QDBusPendingReply<> reply = *aWatcher;
  aWatcher->deleteLater();
  if (reply.isError()) {
    if (mOwner) {
      mOwner->mSessions[aHandle.get()] = SessionState::Error;
    }
    RejectOpError(*aPromise, reply.error().message());
    return;
  }
  if (mOwner) {
    mOwner->mSessions.erase(aHandle.get());
  }
  aPromise->MaybeResolve(true);
}

void NativeMessagingPortalBackend::GetManifest(
    const nsCString& aHandle, const nsCString& aName,
    const nsCString& aExtension, const RefPtr<dom::Promise>& aPromise) {
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
      QString::fromLatin1(kWebExtIface), QStringLiteral("GetManifest"));
  msg << QString::fromUtf8(aHandle.get()) << QString::fromUtf8(aName.get())
      << QString::fromUtf8(aExtension.get());
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(msg, 15000), this);
  RefPtr<dom::Promise> promise = aPromise;
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [promise](QDBusPendingCallWatcher* w) {
            QDBusPendingReply<QString> reply = *w;
            w->deleteLater();
            if (reply.isError()) {
              RejectOpError(*promise, reply.error().message());
              return;
            }
            promise->MaybeResolve(
                nsDependentCString(reply.value().toUtf8().constData()));
          });
}

void NativeMessagingPortalBackend::Start(
    const nsCString& aHandle, const nsCString& aName,
    const nsCString& aExtension, const RefPtr<dom::Promise>& aPromise) {
  nsAutoCString token;
  MakePortalRequestToken(aExtension, token);
  const QString requestPath =
      PortalRequestPath(QString::fromUtf8(token.get()));

  auto* bridge = new NMPResponseBridge(this, aHandle, aPromise);
  bridge->SetRequestPath(requestPath);

  QVariantMap options;
  options.insert(QStringLiteral("handle_token"),
                 QString::fromUtf8(token.get()));
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
      QString::fromLatin1(kWebExtIface), QStringLiteral("Start"));
  msg << QString::fromUtf8(aHandle.get()) << QString::fromUtf8(aName.get())
      << QString::fromUtf8(aExtension.get()) << options;
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(msg, 60000), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [bridge](QDBusPendingCallWatcher* w) {
            QDBusPendingReply<QDBusObjectPath> reply = *w;
            w->deleteLater();
            if (reply.isError()) {
              bridge->Fail(reply.error().message());
              return;
            }
            if (!reply.value().path().isEmpty()) {
              bridge->MaybeRetarget(reply.value().path());
            }
          });
}

void NativeMessagingPortalBackend::GetPipes(
    const nsCString& aHandle, const RefPtr<dom::Promise>& aPromise) {
  QVariantMap options;
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
      QString::fromLatin1(kWebExtIface), QStringLiteral("GetPipes"));
  msg << QString::fromUtf8(aHandle.get()) << options;
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(msg, 15000), this);
  RefPtr<dom::Promise> promise = aPromise;
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [promise](QDBusPendingCallWatcher* w) {
            OnGetPipesFinished(w, promise);
          });
}

void NativeMessagingPortalBackend::OnGetPipesFinished(
    QDBusPendingCallWatcher* aWatcher, RefPtr<dom::Promise> aPromise) {
  // Prefer typed demarshalling; fall back to manual argument walk. Portal
  // GetPipes returns three unix FDs (type 'h'). Some Qt builds leave them as
  // QDBusArgument or nested QDBusVariant rather than QDBusUnixFileDescriptor.
  auto extractOwnedFd = [](QVariant v) -> int {
    int depth = 0;
    while (depth++ < 4 && v.userType() == qMetaTypeId<QDBusVariant>()) {
      v = v.value<QDBusVariant>().variant();
    }
    if (v.canConvert<QDBusUnixFileDescriptor>()) {
      QDBusUnixFileDescriptor ufd = v.value<QDBusUnixFileDescriptor>();
      if (ufd.isValid()) {
        // takeFileDescriptor() transfers ownership so the reply can drop.
        const int fd = ufd.takeFileDescriptor();
        if (fd >= 0) {
          return fd;
        }
      }
    }
    if (v.canConvert<QDBusArgument>()) {
      QDBusArgument arg = v.value<QDBusArgument>();
      // Unix FD signature is "h".
      if (arg.currentSignature() == QLatin1String("h")) {
        QDBusUnixFileDescriptor ufd;
        arg >> ufd;
        if (ufd.isValid()) {
          return ufd.takeFileDescriptor();
        }
      }
    }
    // Last resort: some demarshallers expose a still-open handle as int.
    // Only accept small non-negative values that look like FDs, then dup.
    if (v.canConvert<int>()) {
      bool ok = false;
      const int maybe = v.toInt(&ok);
      if (ok && maybe >= 0 && maybe < 1024) {
        const int duped = fcntl(maybe, F_DUPFD_CLOEXEC, 0);
        if (duped >= 0) {
          return duped;
        }
      }
    }
    return -1;
  };

  auto closeIfOpen = [](int fd) {
    if (fd >= 0) {
      close(fd);
    }
  };

  int stdinFd = -1;
  int stdoutFd = -1;
  int stderrFd = -1;

  {
    QDBusPendingReply<QDBusUnixFileDescriptor, QDBusUnixFileDescriptor,
                      QDBusUnixFileDescriptor>
        typed = *aWatcher;
    if (!typed.isError() && typed.isValid()) {
      auto takeTyped = [](QDBusUnixFileDescriptor ufd) -> int {
        return ufd.isValid() ? ufd.takeFileDescriptor() : -1;
      };
      stdinFd = takeTyped(typed.argumentAt<0>());
      stdoutFd = takeTyped(typed.argumentAt<1>());
      stderrFd = takeTyped(typed.argumentAt<2>());
    }
  }

  if (stdinFd < 0 || stdoutFd < 0 || stderrFd < 0) {
    closeIfOpen(stdinFd);
    closeIfOpen(stdoutFd);
    closeIfOpen(stderrFd);
    stdinFd = stdoutFd = stderrFd = -1;

    const QDBusMessage replyMsg = aWatcher->reply();
    if (replyMsg.type() == QDBusMessage::ErrorMessage) {
      aWatcher->deleteLater();
      RejectOpError(*aPromise, replyMsg.errorMessage());
      return;
    }
    const QList<QVariant> args = replyMsg.arguments();
    LOG_NMP("GetPipes arg count=%d types=[%s,%s,%s]", int(args.size()),
            args.size() > 0 ? args[0].typeName() : "-",
            args.size() > 1 ? args[1].typeName() : "-",
            args.size() > 2 ? args[2].typeName() : "-");
    stdinFd = args.size() > 0 ? extractOwnedFd(args[0]) : -1;
    stdoutFd = args.size() > 1 ? extractOwnedFd(args[1]) : -1;
    stderrFd = args.size() > 2 ? extractOwnedFd(args[2]) : -1;
  }

  aWatcher->deleteLater();

  LOG_NMP("GetPipes fds: %d %d %d", stdinFd, stdoutFd, stderrFd);
  if (stdinFd < 0 || stdoutFd < 0 || stderrFd < 0) {
    closeIfOpen(stdinFd);
    closeIfOpen(stdoutFd);
    closeIfOpen(stderrFd);
    RejectOpError(*aPromise, QStringLiteral("Invalid file descriptors"));
    return;
  }

  // Ensure CLOEXEC on owned FDs that may not already have it (takeFileDescriptor).
  auto ensureCloexec = [](int fd) -> int {
    if (fd < 0) {
      return -1;
    }
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0 && !(flags & FD_CLOEXEC)) {
      fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    return fd;
  };
  stdinFd = ensureCloexec(stdinFd);
  stdoutFd = ensureCloexec(stdoutFd);
  stderrFd = ensureCloexec(stderrFd);

  dom::AutoJSAPI jsapi;
  if (NS_WARN_IF(!jsapi.Init(aPromise->GetGlobalObject()))) {
    closeIfOpen(stdinFd);
    closeIfOpen(stdoutFd);
    closeIfOpen(stderrFd);
    aPromise->MaybeRejectWithUnknownError("Failed to initialize JS context");
    return;
  }
  JSContext* cx = jsapi.cx();
  JS::Rooted<JSObject*> jsPipes(cx, JS_NewPlainObject(cx));
  if (!jsPipes) {
    closeIfOpen(stdinFd);
    closeIfOpen(stdoutFd);
    closeIfOpen(stderrFd);
    aPromise->MaybeRejectWithOperationError("Failed to create pipes object");
    return;
  }
  auto setProp = [&](const char* name, int32_t value) {
    JS::Rooted<JS::Value> jsValue(cx, JS::Value::fromInt32(value));
    return JS_SetProperty(cx, jsPipes, name, jsValue);
  };
  if (!setProp("stdin", stdinFd) || !setProp("stdout", stdoutFd) ||
      !setProp("stderr", stderrFd)) {
    closeIfOpen(stdinFd);
    closeIfOpen(stdoutFd);
    closeIfOpen(stderrFd);
    aPromise->MaybeRejectWithOperationError("Failed to set pipe properties");
    return;
  }
  // Ownership of the three FDs is transferred to JS (native messaging code
  // closes them). Do not close here.
  aPromise->MaybeResolve(jsPipes);
}

void NativeMessagingPortalBackend::SubscribeSessionClosed(
    const QString& aSessionPath) {
  auto* bridge = new NMPSessionClosedBridge(this, aSessionPath);
  if (!QDBusConnection::sessionBus().connect(
          QString::fromLatin1(kPortalService), aSessionPath,
          QString::fromLatin1(kSessionIface), QStringLiteral("Closed"), bridge,
          SLOT(OnClosed()))) {
    delete bridge;
  }
}

// --- bridges ---

NMPResponseBridge::NMPResponseBridge(NativeMessagingPortalBackend* aBackend,
                                     const nsCString& aHandle,
                                     RefPtr<dom::Promise> aPromise)
    : QObject(aBackend),
      mBackend(aBackend),
      mHandle(aHandle),
      mPromise(std::move(aPromise)) {}

void NMPResponseBridge::SetRequestPath(const QString& aPath) {
  mPath = aPath;
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kPortalService), mPath,
      QString::fromLatin1(kRequestIface), QStringLiteral("Response"), this,
      SLOT(OnResponse(uint,QVariantMap)));
}

void NMPResponseBridge::MaybeRetarget(const QString& aNewPath) {
  if (mDone || aNewPath == mPath) {
    return;
  }
  Disconnect();
  SetRequestPath(aNewPath);
}

void NMPResponseBridge::Fail(const QString& aMsg) {
  if (mDone) {
    return;
  }
  mDone = true;
  Disconnect();
  RejectOpError(*mPromise, aMsg);
  deleteLater();
}

void NMPResponseBridge::OnResponse(uint aResponse, const QVariantMap&) {
  if (mDone) {
    return;
  }
  mDone = true;
  Disconnect();
  if (aResponse == 0) {
    LOG_NMP("Start approved; GetPipes for %s", mHandle.get());
    if (mBackend) {
      mBackend->GetPipes(mHandle, mPromise);
    }
  } else if (aResponse == 1) {
    mPromise->MaybeRejectWithAbortError(
        "Native application start canceled by user");
  } else {
    mPromise->MaybeRejectWithNotFoundError("Native application start failed");
  }
  deleteLater();
}

void NMPResponseBridge::Disconnect() {
  if (!mPath.isEmpty()) {
    QDBusConnection::sessionBus().disconnect(
        QString::fromLatin1(kPortalService), mPath,
        QString::fromLatin1(kRequestIface), QStringLiteral("Response"), this,
        SLOT(OnResponse(uint,QVariantMap)));
    mPath.clear();
  }
}

NMPSessionClosedBridge::NMPSessionClosedBridge(
    NativeMessagingPortalBackend* aBackend, const QString& aPath)
    : QObject(aBackend), mBackend(aBackend), mPath(aPath) {}

void NMPSessionClosedBridge::OnClosed() {
  LOG_NMP("session %s closed by portal", qPrintable(mPath));
  if (mBackend && mBackend->mOwner) {
    mBackend->mOwner->mSessions.erase(mPath.toStdString());
  }
  QDBusConnection::sessionBus().disconnect(
      QString::fromLatin1(kPortalService), mPath,
      QString::fromLatin1(kSessionIface), QStringLiteral("Closed"), this,
      SLOT(OnClosed()));
  deleteLater();
}

}  // namespace mozilla::extensions
