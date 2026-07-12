/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_WakeLockListener_h
#define widget_qt_WakeLockListener_h

#include "mozilla/Maybe.h"
#include "nsHashKeys.h"
#include "nsIDOMWakeLockListener.h"
#include "nsRefPtrHashtable.h"
#include "nsString.h"

namespace mozilla::widget {

/**
 * Maps Gecko wake-lock topics to freedesktop D-Bus inhibit cookies via QtDBus
 * (ScreenSaver and PowerManagement.Inhibit). Enough for media playback so
 * the session does not blank the screen or suspend.
 */
class WakeLockListener final : public nsIDOMMozWakeLockListener {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIDOMMOZWAKELOCKLISTENER

  WakeLockListener();

 private:
  ~WakeLockListener();

  class TopicState final {
   public:
    NS_INLINE_DECL_REFCOUNTING(TopicState)

    explicit TopicState(const nsACString& aTopic) : mTopic(aTopic) {}

    nsresult Inhibit();
    nsresult Uninhibit();
    bool IsInhibited() const { return mCookie.isSome(); }

   private:
    ~TopicState() { (void)Uninhibit(); }

    nsCString mTopic;
    // Cookie from org.freedesktop.ScreenSaver.Inhibit
    mozilla::Maybe<uint32_t> mCookie;
    // Cookie from org.freedesktop.PowerManagement.Inhibit
    mozilla::Maybe<uint32_t> mPowerCookie;
  };

  nsRefPtrHashtable<nsCStringHashKey, TopicState> mTopics;
};

}  // namespace mozilla::widget

#endif
