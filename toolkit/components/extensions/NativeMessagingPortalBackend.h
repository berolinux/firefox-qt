/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_extensions_NativeMessagingPortalBackend_h
#define mozilla_extensions_NativeMessagingPortalBackend_h

#include "NativeMessagingPortal.h"
#include "mozilla/dom/Promise.h"
#include "nsString.h"

#pragma GCC visibility push(default)
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusObjectPath>
#pragma GCC visibility pop

namespace mozilla::extensions {

class NativeMessagingPortalBackend final : public QObject {
  Q_OBJECT
 public:
  explicit NativeMessagingPortalBackend(NativeMessagingPortal* aOwner);
  void ClearOwner() { mOwner = nullptr; }

  void ProbeAvailability();
  void CreateSession(const nsCString& aToken, const RefPtr<dom::Promise>& aPromise);
  void CloseSession(const nsCString& aHandle,
                    const RefPtr<dom::Promise>& aPromise);
  void GetManifest(const nsCString& aHandle, const nsCString& aName,
                   const nsCString& aExtension,
                   const RefPtr<dom::Promise>& aPromise);
  void Start(const nsCString& aHandle, const nsCString& aName,
             const nsCString& aExtension, const RefPtr<dom::Promise>& aPromise);
  void GetPipes(const nsCString& aHandle, const RefPtr<dom::Promise>& aPromise);

 private Q_SLOTS:
  void OnVersionFinished(QDBusPendingCallWatcher* aWatcher);

 private:
  friend class NMPResponseBridge;
  friend class NMPSessionClosedBridge;

  void SubscribeSessionClosed(const QString& aSessionPath);
  void OnCreateSessionFinished(QDBusPendingCallWatcher* aWatcher,
                               RefPtr<dom::Promise> aPromise);
  void OnCloseSessionFinished(QDBusPendingCallWatcher* aWatcher,
                              RefPtr<dom::Promise> aPromise,
                              nsCString aHandle);
  static void OnGetPipesFinished(QDBusPendingCallWatcher* aWatcher,
                                 RefPtr<dom::Promise> aPromise);

 public:
  NativeMessagingPortal* mOwner;
};

class NMPResponseBridge final : public QObject {
  Q_OBJECT
 public:
  NMPResponseBridge(NativeMessagingPortalBackend* aBackend,
                    const nsCString& aHandle, RefPtr<dom::Promise> aPromise);
  void Fail(const QString& aMsg);
  void MaybeRetarget(const QString& aNewPath);
  void SetRequestPath(const QString& aPath);

 public Q_SLOTS:
  void OnResponse(uint aResponse, const QVariantMap& aResults);

 private:
  void Disconnect();
  NativeMessagingPortalBackend* mBackend;
  nsCString mHandle;
  RefPtr<dom::Promise> mPromise;
  QString mPath;
  bool mDone = false;
};

class NMPSessionClosedBridge final : public QObject {
  Q_OBJECT
 public:
  NMPSessionClosedBridge(NativeMessagingPortalBackend* aBackend,
                         const QString& aPath);

 public Q_SLOTS:
  void OnClosed();

 private:
  NativeMessagingPortalBackend* mBackend;
  QString mPath;
};

}  // namespace mozilla::extensions

#endif
