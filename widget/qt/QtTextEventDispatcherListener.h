/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_QtTextEventDispatcherListener_h
#define widget_qt_QtTextEventDispatcherListener_h

#include "mozilla/EventForwards.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/TextEventDispatcherListener.h"

namespace mozilla::widget {

/**
 * Routes IME notifications from Gecko to the focused Qt nsWindow and tells
 * Gecko which IME notifications we need (selection / position / text).
 */
class QtTextEventDispatcherListener final : public TextEventDispatcherListener {
 public:
  static QtTextEventDispatcherListener* GetInstance();
  static void Shutdown();

  NS_DECL_ISUPPORTS

  NS_IMETHOD NotifyIME(TextEventDispatcher* aTextEventDispatcher,
                       const IMENotification& aNotification) override;
  NS_IMETHOD_(IMENotificationRequests) GetIMENotificationRequests() override;
  NS_IMETHOD_(void)
  OnRemovedFrom(TextEventDispatcher* aTextEventDispatcher) override;
  NS_IMETHOD_(void)
  WillDispatchKeyboardEvent(TextEventDispatcher* aTextEventDispatcher,
                            WidgetKeyboardEvent& aKeyboardEvent,
                            uint32_t aIndexOfKeypress, void* aData) override;

 private:
  QtTextEventDispatcherListener() = default;
  ~QtTextEventDispatcherListener() = default;

  static StaticRefPtr<QtTextEventDispatcherListener> sInstance;
};

}  // namespace mozilla::widget

#endif
