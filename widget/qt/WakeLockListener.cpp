/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WakeLockListener.h"

#include "mozilla/Logging.h"
#include "mozilla/ScopeExit.h"
#include "nsContentUtils.h"
#include "nsReadableUtils.h"

#pragma GCC visibility push(default)
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#pragma GCC visibility pop

using namespace mozilla;
using namespace mozilla::widget;

static LazyLogModule gQtWakeLockLog("QtWakeLock");
#define WLOG(...) \
  MOZ_LOG(gQtWakeLockLog, LogLevel::Debug, ("WakeLock: " __VA_ARGS__))

NS_IMPL_ISUPPORTS(WakeLockListener, nsIDOMMozWakeLockListener)

static nsCString NiceReason(const nsACString& aTopic) {
  // Short human-readable reason for desktop shells / inhibit UIs.
  if (aTopic.EqualsLiteral("video-playing")) {
    return "Playing video"_ns;
  }
  if (aTopic.EqualsLiteral("audio-playing")) {
    return "Playing audio"_ns;
  }
  if (aTopic.EqualsLiteral("screen")) {
    return "Screen lock"_ns;
  }
  if (aTopic.EqualsLiteral("autoscroll")) {
    return "Autoscroll"_ns;
  }
  return nsCString(aTopic);
}

nsresult WakeLockListener::TopicState::Inhibit() {
  if (mCookie || mPowerCookie) {
    return NS_OK;
  }

  if (!QDBusConnection::sessionBus().isConnected()) {
    WLOG("session bus not connected");
    return NS_ERROR_FAILURE;
  }

  const QString app = QStringLiteral("firefox");
  const nsCString nice = NiceReason(mTopic);
  const QString reason = QString::fromUtf8(nice.get(), nice.Length());

  // Primary: freedesktop ScreenSaver (KDE, GNOME, many WMs)
  {
    QDBusInterface iface(QStringLiteral("org.freedesktop.ScreenSaver"),
                         QStringLiteral("/ScreenSaver"),
                         QStringLiteral("org.freedesktop.ScreenSaver"),
                         QDBusConnection::sessionBus());
    if (iface.isValid()) {
      QDBusReply<uint> reply = iface.call(QStringLiteral("Inhibit"), app, reason);
      if (reply.isValid()) {
        mCookie = Some(reply.value());
        WLOG("ScreenSaver Inhibit cookie=%u topic=%s", *mCookie, mTopic.get());
      } else {
        WLOG("ScreenSaver Inhibit failed: %s",
             qPrintable(reply.error().message()));
      }
    }
  }

  // Secondary: PowerManagement (some desktops only honor this)
  {
    QDBusInterface iface(
        QStringLiteral("org.freedesktop.PowerManagement"),
        QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
        QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
        QDBusConnection::sessionBus());
    if (iface.isValid()) {
      QDBusReply<uint> reply = iface.call(QStringLiteral("Inhibit"), app, reason);
      if (reply.isValid()) {
        mPowerCookie = Some(reply.value());
        WLOG("PowerManagement Inhibit cookie=%u topic=%s", *mPowerCookie,
             mTopic.get());
      }
    }
  }

  return (mCookie || mPowerCookie) ? NS_OK : NS_ERROR_FAILURE;
}

nsresult WakeLockListener::TopicState::Uninhibit() {
  if (mCookie) {
    QDBusInterface iface(QStringLiteral("org.freedesktop.ScreenSaver"),
                         QStringLiteral("/ScreenSaver"),
                         QStringLiteral("org.freedesktop.ScreenSaver"),
                         QDBusConnection::sessionBus());
    if (iface.isValid()) {
      iface.call(QStringLiteral("UnInhibit"), *mCookie);
      WLOG("ScreenSaver UnInhibit cookie=%u", *mCookie);
    }
    mCookie.reset();
  }
  if (mPowerCookie) {
    QDBusInterface iface(
        QStringLiteral("org.freedesktop.PowerManagement"),
        QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
        QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
        QDBusConnection::sessionBus());
    if (iface.isValid()) {
      iface.call(QStringLiteral("UnInhibit"), *mPowerCookie);
      WLOG("PowerManagement UnInhibit cookie=%u", *mPowerCookie);
    }
    mPowerCookie.reset();
  }
  return NS_OK;
}

WakeLockListener::WakeLockListener() {
  WLOG("created");
}

WakeLockListener::~WakeLockListener() {
  WLOG("destroyed");
  for (const auto& entry : mTopics) {
    entry.GetData()->Uninhibit();
  }
  mTopics.Clear();
}

NS_IMETHODIMP
WakeLockListener::Callback(const nsAString& aTopic, const nsAString& aState) {
  if (!aTopic.Equals(u"screen"_ns) && !aTopic.Equals(u"video-playing"_ns) &&
      !aTopic.Equals(u"autoscroll"_ns) && !aTopic.Equals(u"audio-playing"_ns)) {
    return NS_OK;
  }

  const bool shouldLock = aState.EqualsLiteral("locked-background") ||
                          aState.EqualsLiteral("locked-foreground");

  NS_ConvertUTF16toUTF8 topicUtf8(aTopic);
  WLOG("Callback topic=%s state=%s lock=%d", topicUtf8.get(),
       NS_ConvertUTF16toUTF8(aState).get(), shouldLock);

  RefPtr<TopicState> state = mTopics.Get(topicUtf8);
  if (shouldLock) {
    if (!state) {
      state = new TopicState(topicUtf8);
      mTopics.InsertOrUpdate(topicUtf8, RefPtr{state});
    }
    (void)state->Inhibit();
  } else if (state) {
    (void)state->Uninhibit();
  }
  return NS_OK;
}
