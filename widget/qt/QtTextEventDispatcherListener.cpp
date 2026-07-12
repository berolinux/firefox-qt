/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "QtTextEventDispatcherListener.h"

#include "mozilla/TextEventDispatcher.h"
#include "mozilla/widget/IMEData.h"
#include "nsWindow.h"

#pragma GCC visibility push(default)
#include <QGuiApplication>
#include <QInputMethod>
#pragma GCC visibility pop

namespace mozilla::widget {

StaticRefPtr<QtTextEventDispatcherListener>
    QtTextEventDispatcherListener::sInstance;

// static
QtTextEventDispatcherListener* QtTextEventDispatcherListener::GetInstance() {
  if (!sInstance) {
    sInstance = new QtTextEventDispatcherListener();
  }
  return sInstance.get();
}

// static
void QtTextEventDispatcherListener::Shutdown() { sInstance = nullptr; }

NS_IMPL_ISUPPORTS(QtTextEventDispatcherListener, TextEventDispatcherListener,
                  nsISupportsWeakReference)

NS_IMETHODIMP
QtTextEventDispatcherListener::NotifyIME(
    TextEventDispatcher* aTextEventDispatcher,
    const IMENotification& aNotification) {
  nsWindow* window =
      nsWindow::FromWidget(aTextEventDispatcher->GetWidget());
  if (!window) {
    return NS_OK;
  }

  switch (aNotification.mMessage) {
    case REQUEST_TO_COMMIT_COMPOSITION:
      window->RequestIMECommitComposition();
      break;
    case REQUEST_TO_CANCEL_COMPOSITION:
      window->RequestIMECancelComposition();
      break;
    case NOTIFY_IME_OF_FOCUS:
      window->NotifyIMEOfFocusChange(true);
      break;
    case NOTIFY_IME_OF_BLUR:
      window->NotifyIMEOfFocusChange(false);
      break;
    case NOTIFY_IME_OF_SELECTION_CHANGE:
      window->NotifyIMEOfSelectionChange(aNotification);
      break;
    case NOTIFY_IME_OF_TEXT_CHANGE:
      window->NotifyIMEOfTextChange(aNotification);
      break;
    case NOTIFY_IME_OF_POSITION_CHANGE:
    case NOTIFY_IME_OF_COMPOSITION_EVENT_HANDLED:
      window->NotifyIMEOfPositionChange();
      break;
    default:
      break;
  }
  return NS_OK;
}

NS_IMETHODIMP_(IMENotificationRequests)
QtTextEventDispatcherListener::GetIMENotificationRequests() {
  // Caret rect + surrounding text need position and text-change updates so
  // candidate windows track the caret and IM queries stay fresh.
  return IMENotificationRequests{
      IMENotificationRequest::TextChange,
      IMENotificationRequest::PositionChange,
  };
}

NS_IMETHODIMP_(void)
QtTextEventDispatcherListener::OnRemovedFrom(TextEventDispatcher*) {}

NS_IMETHODIMP_(void)
QtTextEventDispatcherListener::WillDispatchKeyboardEvent(
    TextEventDispatcher*, WidgetKeyboardEvent&, uint32_t, void*) {}

}  // namespace mozilla::widget
