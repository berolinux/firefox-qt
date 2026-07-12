/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_extensions_NativeMessagingPortal_h
#define mozilla_extensions_NativeMessagingPortal_h

// GTK builds keep the GIO/GDBus implementation; Qt uses QtDBus.
#if defined(MOZ_WIDGET_GTK)
#  include "NativeMessagingPortalGio.h"
#else

#  include "nsINativeMessagingPortal.h"
#  include "mozilla/UniquePtr.h"
#  include "nsString.h"

#  include <deque>
#  include <string>
#  include <unordered_map>

namespace mozilla {
namespace dom {
class Promise;
}
namespace extensions {

enum class SessionState { Active, Closing, Error };

class NativeMessagingPortalBackend;

/**
 * WebExtensions xdg-desktop-portal client using QtDBus (no GIO).
 * Interface: org.freedesktop.portal.WebExtensions
 */
class NativeMessagingPortal final : public nsINativeMessagingPortal {
 public:
  NS_DECL_NSINATIVEMESSAGINGPORTAL
  NS_DECL_ISUPPORTS

  static already_AddRefed<NativeMessagingPortal> GetSingleton();

  bool mReady = false;
  bool mAvailable = false;
  using SessionsMap = std::unordered_map<std::string, SessionState>;
  SessionsMap mSessions;
  void DrainPending();

 private:
  friend class NativeMessagingPortalBackend;
  friend class NMPResponseBridge;
  friend class NMPSessionClosedBridge;

  NativeMessagingPortal();
  ~NativeMessagingPortal();

  struct DelayedCall {
    enum class Kind { Available, CreateSession };
    Kind kind = Kind::Available;
    RefPtr<dom::Promise> promise;
    nsCString token;
  };
  std::deque<UniquePtr<DelayedCall>> mPending;

  void DoIsAvailable(dom::Promise& aPromise);
  void DoCreateSession(dom::Promise& aPromise, const nsACString& aToken);

  UniquePtr<NativeMessagingPortalBackend> mBackend;
};

}  // namespace extensions
}  // namespace mozilla

#endif  // !MOZ_WIDGET_GTK

#endif  // mozilla_extensions_NativeMessagingPortal_h
