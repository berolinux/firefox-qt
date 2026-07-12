/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "QtWaylandVsyncSource.h"

#include "MainThreadUtils.h"
#include "mozilla/StaticPrefs_layout.h"
#include "mozilla/widget/WindowOcclusionState.h"
#include "nsWindow.h"
#include "QtWindowBridge.h"

#include <wayland-client.h>

#pragma GCC visibility push(default)
#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#pragma GCC visibility pop

namespace mozilla {

using widget::OcclusionState;

namespace {

const struct wl_callback_listener kFrameListener = {
    // done
    [](void* data, wl_callback* callback, uint32_t time) {
      auto* self = static_cast<QtWaylandVsyncSource*>(data);
      // Wayland dispatches on the Qt event thread (main).
      self->FrameCallback(time);
      wl_callback_destroy(callback);
    },
};

}  // namespace

QtWaylandVsyncSource::QtWaylandVsyncSource(nsWindow* aWindow)
    : mMutex("QtWaylandVsyncSource"),
      mWindow(aWindow),
      mVsyncRate(TimeDuration::FromMilliseconds(1000.0 / 60.0)),
      mLastVsyncTimeStamp(TimeStamp::Now()) {
  MOZ_ASSERT(NS_IsMainThread());
}

QtWaylandVsyncSource::~QtWaylandVsyncSource() {
  MutexAutoLock lock(mMutex);
  if (mCallback) {
    wl_callback_destroy(mCallback);
    mCallback = nullptr;
  }
}

TimeDuration QtWaylandVsyncSource::GetVsyncRate() {
  MutexAutoLock lock(mMutex);
  return mVsyncRate;
}

void QtWaylandVsyncSource::CalculateVsyncRateLocked(
    const MutexAutoLock& /*aProofOfLock*/, TimeStamp aNow) {
  if (!mLastVsyncTimeStamp.IsNull()) {
    const TimeDuration delta = aNow - mLastVsyncTimeStamp;
    // EMA toward observed interval; clamp to ~30–360 Hz.
    if (delta.ToMilliseconds() > 2.0 && delta.ToMilliseconds() < 40.0) {
      mVsyncRate = TimeDuration::FromMilliseconds(
          mVsyncRate.ToMilliseconds() * 0.7 + delta.ToMilliseconds() * 0.3);
    }
  }
}

void QtWaylandVsyncSource::RequestFrameCallbackLocked(
    const MutexAutoLock& /*aProofOfLock*/) {
  if (mCallbackPending || !mWindow || mIsShutdown) {
    return;
  }
  QtWindowBridge* qw = mWindow->GetQtWindow();
  if (!qw) {
    return;
  }
  if (!qw->handle()) {
    qw->create();
  }
  QPlatformNativeInterface* native = QGuiApplication::platformNativeInterface();
  if (!native) {
    return;
  }
  auto* surface = static_cast<wl_surface*>(
      native->nativeResourceForWindow("surface", qw));
  if (!surface) {
    return;
  }

  mCallback = wl_surface_frame(surface);
  if (!mCallback) {
    return;
  }
  wl_callback_add_listener(mCallback, &kFrameListener, this);
  mCallbackPending = true;
  // Empty commit so the compositor schedules the next frame callback.
  wl_surface_commit(surface);
}

void QtWaylandVsyncSource::FrameCallback(uint32_t aTime) {
  MOZ_ASSERT(NS_IsMainThread());

  TimeStamp lastVSync;
  TimeStamp outputTimestamp;
  RefPtr<nsWindow> window;
  {
    MutexAutoLock lock(mMutex);
    mCallback = nullptr;
    mCallbackPending = false;

    if (mIsShutdown || !mVsyncEnabled || !mVsyncSourceEnabled) {
      return;
    }
    if (aTime && mLastFrameTime == aTime) {
      RequestFrameCallbackLocked(lock);
      return;
    }
    mLastFrameTime = aTime;

    const TimeStamp now = TimeStamp::Now();
    CalculateVsyncRateLocked(lock, now);
    mLastVsyncTimeStamp = lastVSync = now;
    outputTimestamp = mLastVsyncTimeStamp + mVsyncRate;
    window = mWindow;
  }

  if (window) {
    window->NotifyOcclusionState(OcclusionState::VISIBLE);
    if (window->Destroyed()) {
      return;
    }
  }

  NotifyVsync(lastVSync, outputTimestamp);

  MutexAutoLock lock(mMutex);
  if (mVsyncEnabled && mVsyncSourceEnabled && !mIsShutdown) {
    RequestFrameCallbackLocked(lock);
  }
}

void QtWaylandVsyncSource::EnableVsync() {
  MOZ_ASSERT(NS_IsMainThread());
  MutexAutoLock lock(mMutex);
  if (mVsyncEnabled || mIsShutdown) {
    return;
  }
  mVsyncEnabled = true;
  if (mVsyncSourceEnabled) {
    RequestFrameCallbackLocked(lock);
  }
}

void QtWaylandVsyncSource::DisableVsync() {
  MOZ_ASSERT(NS_IsMainThread());
  MutexAutoLock lock(mMutex);
  mVsyncEnabled = false;
}

bool QtWaylandVsyncSource::IsVsyncEnabled() {
  MutexAutoLock lock(mMutex);
  return mVsyncEnabled;
}

void QtWaylandVsyncSource::EnableVSyncSource() {
  MutexAutoLock lock(mMutex);
  mVsyncSourceEnabled = true;
  if (mVsyncEnabled && !mIsShutdown) {
    RequestFrameCallbackLocked(lock);
  }
}

void QtWaylandVsyncSource::DisableVSyncSource() {
  MutexAutoLock lock(mMutex);
  mVsyncSourceEnabled = false;
}

void QtWaylandVsyncSource::Shutdown() {
  MOZ_ASSERT(NS_IsMainThread());
  MutexAutoLock lock(mMutex);
  mIsShutdown = true;
  mVsyncEnabled = false;
  mVsyncSourceEnabled = false;
  if (mCallback) {
    wl_callback_destroy(mCallback);
    mCallback = nullptr;
  }
  mCallbackPending = false;
  mWindow = nullptr;
}

}  // namespace mozilla
