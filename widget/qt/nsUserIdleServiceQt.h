/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_nsUserIdleServiceQt_h
#define widget_qt_nsUserIdleServiceQt_h

#include "nsUserIdleService.h"
#include "mozilla/AppShutdown.h"

class nsUserIdleServiceQt : public nsUserIdleService {
 public:
  NS_INLINE_DECL_REFCOUNTING_INHERITED(nsUserIdleServiceQt, nsUserIdleService)

  bool PollIdleTime(uint32_t* aIdleTime) override;

  // Called from nsWindow input handlers so idle time tracks real use.
  static void NotifyUserActivity();

  static already_AddRefed<nsUserIdleServiceQt> GetInstance() {
    RefPtr<nsUserIdleService> idleService = nsUserIdleService::GetInstance();
    if (!idleService) {
      if (mozilla::AppShutdown::IsInOrBeyond(
              mozilla::ShutdownPhase::AppShutdownConfirmed)) {
        return nullptr;
      }
      idleService = new nsUserIdleServiceQt();
    }
    return idleService.forget().downcast<nsUserIdleServiceQt>();
  }

 protected:
  nsUserIdleServiceQt() = default;
  virtual ~nsUserIdleServiceQt() = default;
};

#endif
