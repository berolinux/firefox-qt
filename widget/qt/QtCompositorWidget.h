/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_QtCompositorWidget_h
#define widget_qt_QtCompositorWidget_h

#include "mozilla/widget/CompositorWidget.h"
#include "mozilla/Atomics.h"
#include "mozilla/DataMutex.h"
#include "GLDefs.h"
#include "Units.h"
#include "mozilla/RefPtr.h"

class nsWindow;

namespace mozilla {
namespace gfx {
class DataSourceSurface;
class DrawTarget;
}  // namespace gfx
namespace widget {

class QtCompositorWidget;
class QtCompositorWidgetInitData;

class PlatformCompositorWidgetDelegate : public CompositorWidgetDelegate {
 public:
  virtual void NotifyClientSizeChanged(
      const LayoutDeviceIntSize& aClientSize) = 0;
  virtual void NotifyFullscreenChanged(bool aIsFullscreen) = 0;
  virtual void CleanupResources() = 0;
  virtual QtCompositorWidget* AsQtCompositorWidget() { return nullptr; }

  PlatformCompositorWidgetDelegate* AsPlatformSpecificDelegate() override {
    return this;
  }
};

class QtCompositorWidget : public CompositorWidget,
                           public PlatformCompositorWidgetDelegate {
 public:
  QtCompositorWidget(const QtCompositorWidgetInitData& aInitData,
                     const layers::CompositorOptions& aOptions,
                     RefPtr<nsWindow> aWindow);
  ~QtCompositorWidget() override;

  already_AddRefed<gfx::DrawTarget> StartRemoteDrawing() override;
  already_AddRefed<gfx::DrawTarget> StartRemoteDrawingInRegion(
      const LayoutDeviceIntRegion& aInvalidRegion) override;
  void EndRemoteDrawing() override;
  void EndRemoteDrawingInRegion(
      gfx::DrawTarget* aDrawTarget,
      const LayoutDeviceIntRegion& aInvalidRegion) override;

  LayoutDeviceIntSize GetClientSize() override;
  nsIWidget* RealWidget() override;
  QtCompositorWidget* AsQt() override { return this; }
  CompositorWidgetDelegate* AsDelegate() override { return this; }

  EGLNativeWindowType GetEGLNativeWindow();
  // Keep wl_egl_window size in sync before eglSwapBuffers (Wayland).
  void SetEGLNativeWindowSize(const LayoutDeviceIntSize& aSize);

  void NotifyClientSizeChanged(const LayoutDeviceIntSize& aClientSize) override;
  void NotifyFullscreenChanged(bool aIsFullscreen) override;
  void CleanupResources() override;
  QtCompositorWidget* AsQtCompositorWidget() override { return this; }

  bool IsHidden() const override;
  uintptr_t GetWidgetKey() override;

  // Present a software frame snapshot on the main thread (QWindow is not MT-safe).
  void PresentSoftwareBuffer(gfx::DataSourceSurface* aFrame,
                             const LayoutDeviceIntRegion& aDamage);

 protected:
  RefPtr<nsWindow> mWidget;
  DataMutex<LayoutDeviceIntSize> mClientSize;
  uintptr_t mNativeWindowHandle = 0;
  bool mIsX11Display = true;

  // Software path: triple-buffered surfaces so we can hand a finished buffer
  // to the main thread without a full-frame memcpy in the common case.
  static constexpr int kBufferCount = 3;
  RefPtr<gfx::DataSourceSurface> mBuffers[kBufferCount];
  RefPtr<gfx::DrawTarget> mDrawTargets[kBufferCount];
  LayoutDeviceIntSize mBufferSize;
  int mDrawIndex = 0;
  // Bit i set while buffer i is held by pending present or main-thread paint.
  Atomic<uint32_t> mBufferBusyBits{0};

  struct PendingFrame {
    RefPtr<gfx::DataSourceSurface> mSurface;
    LayoutDeviceIntRegion mDamage;
    int mBufferIndex = -1;
  };
  DataMutex<PendingFrame> mPendingPresent{"QtCompositorWidget::mPendingPresent"};
  Atomic<bool> mPresentPosted{false};
  uint32_t mSwPresentWhileHardware = 0;

  bool EnsureDrawBuffer(const LayoutDeviceIntSize& aSize);
  int PickDrawBufferIndex();
};

}  // namespace widget
}  // namespace mozilla

#endif
