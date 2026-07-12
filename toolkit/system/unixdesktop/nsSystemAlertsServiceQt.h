/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsSystemAlertsServiceQt_h_
#define nsSystemAlertsServiceQt_h_

#include "nsIAlertsService.h"
#include "nsCOMPtr.h"
#include "nsTHashMap.h"
#include "nsString.h"
#include "mozilla/UniquePtr.h"

/**
 * FreeDesktop org.freedesktop.Notifications backend for the Qt toolkit.
 * Registered as @mozilla.org/system-alerts-service;1.
 */
class nsSystemAlertsServiceQt final : public nsIAlertsService,
                                      public nsIAlertsDoNotDisturb {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIALERTSSERVICE
  NS_DECL_NSIALERTSDONOTDISTURB

  nsSystemAlertsServiceQt();
  nsresult Init();

  struct PendingAlert {
    nsCOMPtr<nsIObserver> mListener;
    nsString mCookie;
    nsString mName;
    nsCOMPtr<nsIAlertNotification> mNotification;
  };

  void OnNotificationClosed(uint32_t aId, uint32_t aReason);
  void OnActionInvoked(uint32_t aId, const nsACString& aActionKey);

 private:
  ~nsSystemAlertsServiceQt();

  nsresult ShowFreedesktopNotification(nsIAlertNotification* aAlert,
                                       nsIObserver* aListener,
                                       const nsAString& aName);

  bool EnsureConnected();
  static nsCString GetAppName();
  static nsCString GetDesktopEntry();

  bool mSuppressForScreenSharing = false;
  bool mDoNotDisturb = false;
  bool mConnected = false;

  // notification id -> pending alert
  nsTHashMap<nsUint32HashKey, PendingAlert> mPending;
  // alert name -> notification id (for CloseAlert)
  nsTHashMap<nsStringHashKey, uint32_t> mNameToId;

  // QObject bridge lives in the .cpp (Qt headers).
  void* mDBusBridge = nullptr;  // owned FdoNotificationBridge*
};

#endif
