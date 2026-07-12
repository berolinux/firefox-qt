/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_QtWaylandVsyncSource_h
#define widget_qt_QtWaylandVsyncSource_h

#include "mozilla/Mutex.h"
#include "mozilla/TimeStamp.h"
#include "VsyncSource.h"

class nsWindow;
struct wl_callback;
struct wl_surface;

namespace mozilla {

// Per-window compositor vsync emulated with wl_surface frame callbacks.
// Mirrors widget/gtk/WaylandVsyncSource without GTK/WaylandSurface deps.
class QtWaylandVsyncSource final : public gfx::VsyncSource {
 public:
  explicit QtWaylandVsyncSource(nsWindow* aWindow);
  ~QtWaylandVsyncSource() override;

  TimeDuration GetVsyncRate() override;
  void EnableVsync() override;
  void DisableVsync() override;
  bool IsVsyncEnabled() override;
  void Shutdown() override;

  void EnableVSyncSource();
  void DisableVSyncSource();

  // Called from the wl_callback listener (main thread via Qt event loop).
  void FrameCallback(uint32_t aTime);

 private:
  void RequestFrameCallbackLocked(const MutexAutoLock& aProofOfLock);
  void CalculateVsyncRateLocked(const MutexAutoLock& aProofOfLock,
                                TimeStamp aNow);

  Mutex mMutex;
  // Main thread only except logging.
  RefPtr<nsWindow> mWindow;

  bool mIsShutdown MOZ_GUARDED_BY(mMutex) = false;
  bool mVsyncEnabled MOZ_GUARDED_BY(mMutex) = false;
  bool mVsyncSourceEnabled MOZ_GUARDED_BY(mMutex) = false;
  bool mCallbackPending MOZ_GUARDED_BY(mMutex) = false;

  TimeDuration mVsyncRate MOZ_GUARDED_BY(mMutex);
  TimeStamp mLastVsyncTimeStamp MOZ_GUARDED_BY(mMutex);
  uint32_t mLastFrameTime MOZ_GUARDED_BY(mMutex) = 0;

  wl_callback* mCallback MOZ_GUARDED_BY(mMutex) = nullptr;
};

}  // namespace mozilla

#endif
