/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "NativeMessagingPortal.h"
#include "NativeMessagingPortalBackend.h"

#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Logging.h"
#include "mozilla/XREAppData.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/widget/PortalUtils.h"
#include "xpcpublic.h"

#pragma GCC visibility push(default)
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#pragma GCC visibility pop

static mozilla::LazyLogModule gNativeMessagingPortalLog(
    "NativeMessagingPortal");
#define LOG_NMP(...) \
  MOZ_LOG(gNativeMessagingPortalLog, mozilla::LogLevel::Debug, (__VA_ARGS__))

namespace mozilla::extensions {

namespace {

constexpr const char kPortalService[] = "org.freedesktop.portal.Desktop";
constexpr const char kSessionIface[] = "org.freedesktop.portal.Session";

bool IsObjectPath(const nsACString& aPath) {
  return !aPath.IsEmpty() && aPath[0] == '/';
}

nsresult GetPromise(JSContext* aCx, RefPtr<dom::Promise>& aPromise) {
  nsIGlobalObject* globalObject = xpc::CurrentNativeGlobal(aCx);
  if (NS_WARN_IF(!globalObject)) {
    return NS_ERROR_UNEXPECTED;
  }
  ErrorResult result;
  aPromise = dom::Promise::Create(globalObject, result);
  if (NS_WARN_IF(result.Failed())) {
    return result.StealNSResult();
  }
  return NS_OK;
}

}  // namespace

NS_IMPL_ISUPPORTS(NativeMessagingPortal, nsINativeMessagingPortal)

/* static */
already_AddRefed<NativeMessagingPortal> NativeMessagingPortal::GetSingleton() {
  static StaticRefPtr<NativeMessagingPortal> sInstance;
  if (MOZ_UNLIKELY(!sInstance)) {
    sInstance = new NativeMessagingPortal();
    ClearOnShutdown(&sInstance);
  }
  return do_AddRef(sInstance);
}

NativeMessagingPortal::NativeMessagingPortal() {
  LOG_NMP("NativeMessagingPortal (QtDBus)");
  mBackend = MakeUnique<NativeMessagingPortalBackend>(this);
  if (QDBusConnection::sessionBus().isConnected()) {
    mBackend->ProbeAvailability();
  } else {
    mReady = true;
    mAvailable = false;
  }
}

NativeMessagingPortal::~NativeMessagingPortal() {
  LOG_NMP("~NativeMessagingPortal");
  for (const auto& it : mSessions) {
    if (it.second != SessionState::Active) {
      continue;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), QString::fromStdString(it.first),
        QString::fromLatin1(kSessionIface), QStringLiteral("Close"));
    QDBusConnection::sessionBus().call(msg, QDBus::Block, 2000);
  }
  if (mBackend) {
    mBackend->ClearOwner();
    mBackend = nullptr;
  }
}

void NativeMessagingPortal::DrainPending() {
  while (!mPending.empty()) {
    UniquePtr<DelayedCall> pending = std::move(mPending.front());
    mPending.pop_front();
    switch (pending->kind) {
      case DelayedCall::Kind::Available:
        DoIsAvailable(*pending->promise);
        break;
      case DelayedCall::Kind::CreateSession:
        DoCreateSession(*pending->promise, pending->token);
        break;
    }
  }
}

NS_IMETHODIMP
NativeMessagingPortal::ShouldUse(bool* aResult) {
  *aResult = widget::ShouldUsePortal(widget::PortalKind::NativeMessaging);
  LOG_NMP("will %sbe used", *aResult ? "" : "not ");
  return NS_OK;
}

NS_IMETHODIMP
NativeMessagingPortal::GetAvailable(JSContext* aCx, dom::Promise** aPromise) {
  RefPtr<dom::Promise> promise;
  MOZ_TRY(GetPromise(aCx, promise));
  if (mReady) {
    DoIsAvailable(*promise);
  } else {
    auto delayed = MakeUnique<DelayedCall>();
    delayed->kind = DelayedCall::Kind::Available;
    delayed->promise = promise;
    mPending.push_back(std::move(delayed));
  }
  promise.forget(aPromise);
  return NS_OK;
}

void NativeMessagingPortal::DoIsAvailable(dom::Promise& aPromise) {
  LOG_NMP("is %savailable", mAvailable ? "" : "not ");
  aPromise.MaybeResolve(mAvailable);
}

NS_IMETHODIMP
NativeMessagingPortal::CreateSession(const nsACString& aApplication,
                                     JSContext* aCx, dom::Promise** aPromise) {
  RefPtr<dom::Promise> promise;
  MOZ_TRY(GetPromise(aCx, promise));
  nsAutoCString token;
  // Local token helper — same rules as portal request tokens.
  static Atomic<unsigned, MemoryOrdering::Relaxed> sSerial;
  token.Truncate();
  token.AppendPrintf("%s_%u", PromiseFlatCString(aApplication).get(),
                     unsigned(sSerial++));
  XREAppData::SanitizeNameForDBus(token);

  if (mReady) {
    DoCreateSession(*promise, token);
  } else {
    auto delayed = MakeUnique<DelayedCall>();
    delayed->kind = DelayedCall::Kind::CreateSession;
    delayed->promise = promise;
    delayed->token = token;
    mPending.push_back(std::move(delayed));
  }
  promise.forget(aPromise);
  return NS_OK;
}

void NativeMessagingPortal::DoCreateSession(dom::Promise& aPromise,
                                            const nsACString& aToken) {
  if (!mAvailable || !mBackend) {
    aPromise.MaybeRejectWithOperationError(
        "No D-Bus proxy for the native messaging portal");
    return;
  }
  LOG_NMP("creating session token %s", PromiseFlatCString(aToken).get());
  // Hold the promise for async D-Bus completion.
  RefPtr<dom::Promise> hold = &aPromise;
  mBackend->CreateSession(nsCString(aToken), hold);
}

NS_IMETHODIMP
NativeMessagingPortal::CloseSession(const nsACString& aHandle, JSContext* aCx,
                                    dom::Promise** aPromise) {
  const nsCString& sessionHandle = PromiseFlatCString(aHandle);
  if (!IsObjectPath(sessionHandle)) {
    return NS_ERROR_INVALID_ARG;
  }
  auto it = mSessions.find(sessionHandle.get());
  if (it == mSessions.end()) {
    return NS_ERROR_INVALID_ARG;
  }
  if (it->second != SessionState::Active) {
    return NS_ERROR_FAILURE;
  }
  RefPtr<dom::Promise> promise;
  MOZ_TRY(GetPromise(aCx, promise));
  it->second = SessionState::Closing;
  LOG_NMP("closing session %s", sessionHandle.get());
  if (mBackend) {
    mBackend->CloseSession(sessionHandle, promise);
  } else {
    promise->MaybeRejectWithOperationError("No backend");
  }
  promise.forget(aPromise);
  return NS_OK;
}

NS_IMETHODIMP
NativeMessagingPortal::GetManifest(const nsACString& aHandle,
                                   const nsACString& aName,
                                   const nsACString& aExtension, JSContext* aCx,
                                   dom::Promise** aPromise) {
  const nsCString& sessionHandle = PromiseFlatCString(aHandle);
  if (!IsObjectPath(sessionHandle)) {
    return NS_ERROR_INVALID_ARG;
  }
  auto it = mSessions.find(sessionHandle.get());
  if (it == mSessions.end()) {
    return NS_ERROR_INVALID_ARG;
  }
  if (it->second != SessionState::Active) {
    return NS_ERROR_FAILURE;
  }
  if (!mBackend || !mAvailable) {
    return NS_ERROR_FAILURE;
  }
  RefPtr<dom::Promise> promise;
  MOZ_TRY(GetPromise(aCx, promise));
  mBackend->GetManifest(sessionHandle, PromiseFlatCString(aName),
                        PromiseFlatCString(aExtension), promise);
  promise.forget(aPromise);
  return NS_OK;
}

NS_IMETHODIMP
NativeMessagingPortal::Start(const nsACString& aHandle, const nsACString& aName,
                             const nsACString& aExtension, JSContext* aCx,
                             dom::Promise** aPromise) {
  const nsCString& sessionHandle = PromiseFlatCString(aHandle);
  if (!IsObjectPath(sessionHandle)) {
    return NS_ERROR_INVALID_ARG;
  }
  auto it = mSessions.find(sessionHandle.get());
  if (it == mSessions.end()) {
    return NS_ERROR_INVALID_ARG;
  }
  if (it->second != SessionState::Active) {
    return NS_ERROR_FAILURE;
  }
  if (!mBackend || !mAvailable) {
    return NS_ERROR_FAILURE;
  }
  RefPtr<dom::Promise> promise;
  MOZ_TRY(GetPromise(aCx, promise));
  LOG_NMP("starting %s for %s in %s", PromiseFlatCString(aName).get(),
          PromiseFlatCString(aExtension).get(), sessionHandle.get());
  mBackend->Start(sessionHandle, PromiseFlatCString(aName),
                  PromiseFlatCString(aExtension), promise);
  promise.forget(aPromise);
  return NS_OK;
}

}  // namespace mozilla::extensions
