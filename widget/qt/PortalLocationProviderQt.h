/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_PortalLocationProviderQt_h
#define mozilla_widget_PortalLocationProviderQt_h

#include "nsCOMPtr.h"
#include "nsIGeolocationProvider.h"
#include "nsITimer.h"
#include "nsString.h"

class MLSFallback;

namespace mozilla::widget {

// org.freedesktop.portal.Location over QtDBus (no GIO).
class PortalLocationProviderQt final : public nsIGeolocationProvider,
                                       public nsITimerCallback,
                                       public nsINamed {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIGEOLOCATIONPROVIDER
  NS_DECL_NSITIMERCALLBACK
  NS_DECL_NSINAMED

  PortalLocationProviderQt();

  void OnLocationUpdated(double aLat, double aLon, double aAlt, double aAccuracy,
                         double aHeading, double aSpeed);

 private:
  ~PortalLocationProviderQt();
  void SetRefreshTimer(int aDelayMs);
  void NotifyError(uint16_t aError);

  nsCOMPtr<nsIGeolocationUpdate> mCallback;
  RefPtr<MLSFallback> mMLSProvider;
  nsCOMPtr<nsIDOMGeoPosition> mLastPosition;
  nsCOMPtr<nsITimer> mRefreshTimer;
  nsCString mSessionPath;
  // Owned QObject for D-Bus signal bridge; deleted in Shutdown.
  void* mBridge = nullptr;
};

}  // namespace mozilla::widget

#endif
