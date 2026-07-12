/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "QtCompositorWidget.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Helpers.h"
#include "mozilla/layers/CompositorThread.h"
#include "mozilla/Atomics.h"
#include "mozilla/EventQueue.h"
#include "nsProxyRelease.h"
#include "nsThreadUtils.h"
#include "nsWindow.h"
#include "QtWindowBridge.h"
#include "nsIWidget.h"
#include <cmath>
#include <cstring>
#include <utility>

#pragma GCC visibility push(default)
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRegion>
#include <QWindow>
#pragma GCC visibility pop

namespace mozilla::widget {

QtCompositorWidget::QtCompositorWidget(
    const QtCompositorWidgetInitData& aInitData,
    const layers::CompositorOptions& aOptions, RefPtr<nsWindow> aWindow)
    : CompositorWidget(aOptions),
      mWidget(std::move(aWindow)),
      mClientSize(LayoutDeviceIntSize(aInitData.InitialClientSize()),
                  "QtCompositorWidget::mClientSize"),
      mNativeWindowHandle(aInitData.NativeWindowHandle()),
      mIsX11Display(aInitData.IsX11Display()) {}

QtCompositorWidget::~QtCompositorWidget() {
  NS_ReleaseOnMainThread("QtCompositorWidget::mWidget", mWidget.forget());
}

bool QtCompositorWidget::EnsureDrawBuffer(const LayoutDeviceIntSize& aSize) {
  if (mBufferSize != aSize) {
    for (int i = 0; i < kBufferCount; ++i) {
      mBuffers[i] = nullptr;
      mDrawTargets[i] = nullptr;
    }
    mBufferSize = aSize;
    mBufferBusyBits = 0;
    mDrawIndex = 0;
  }
  for (int i = 0; i < kBufferCount; ++i) {
    if (!mBuffers[i]) {
      mBuffers[i] = gfx::Factory::CreateDataSourceSurface(
          gfx::IntSize(aSize.width, aSize.height), gfx::SurfaceFormat::B8G8R8A8,
          /* aZero = */ true);
      mDrawTargets[i] = nullptr;
    }
    if (!mBuffers[i] || !mBuffers[i]->GetData()) {
      return false;
    }
  }
  return true;
}

int QtCompositorWidget::PickDrawBufferIndex() {
  // Prefer a free buffer; fall back to the next slot (may still be busy —
  // EndRemoteDrawing then copies).
  const uint32_t busy = mBufferBusyBits;
  for (int n = 0; n < kBufferCount; ++n) {
    const int i = (mDrawIndex + n) % kBufferCount;
    if ((busy & (1u << i)) == 0) {
      return i;
    }
  }
  return mDrawIndex;
}

already_AddRefed<gfx::DrawTarget> QtCompositorWidget::StartRemoteDrawing() {
  return StartRemoteDrawingInRegion(LayoutDeviceIntRegion(LayoutDeviceIntRect(
      LayoutDeviceIntPoint(), GetClientSize())));
}

already_AddRefed<gfx::DrawTarget>
QtCompositorWidget::StartRemoteDrawingInRegion(
    const LayoutDeviceIntRegion& aInvalidRegion) {
  LayoutDeviceIntSize size = GetClientSize();
  // Fallback to the live widget bounds if the compositor was created before
  // the first size notify (SWGL "no dt" / empty canvas).
  if ((size.width <= 0 || size.height <= 0) && mWidget) {
    size = mWidget->GetBounds().Size();
    if (size.width > 0 && size.height > 0) {
      auto locked = mClientSize.Lock();
      *locked = size;
    }
  }
  if (size.width <= 0 || size.height <= 0) {
    return nullptr;
  }

  if (!EnsureDrawBuffer(size)) {
    return nullptr;
  }

  mDrawIndex = PickDrawBufferIndex();
  RefPtr<gfx::DataSourceSurface> buffer = mBuffers[mDrawIndex];

  // Transparent popups paint rounded panels that don't cover the full window.
  // Clear every frame so stale opaque pixels don't form a black frame.
  if (mWidget &&
      mWidget->GetTransparencyMode() != TransparencyMode::Opaque) {
    gfx::DataSourceSurface::ScopedMap map(buffer,
                                          gfx::DataSourceSurface::WRITE);
    if (map.IsMapped()) {
      memset(map.GetData(), 0, size_t(map.GetStride()) * size_t(size.height));
    }
  }

  if (!mDrawTargets[mDrawIndex]) {
    mDrawTargets[mDrawIndex] = gfx::Factory::CreateDrawTargetForData(
        gfx::BackendType::SKIA, buffer->GetData(),
        gfx::IntSize(size.width, size.height), buffer->Stride(),
        gfx::SurfaceFormat::B8G8R8A8);
  }
  return do_AddRef(mDrawTargets[mDrawIndex]);
}

void QtCompositorWidget::EndRemoteDrawing() {
  EndRemoteDrawingInRegion(mDrawTargets[mDrawIndex], LayoutDeviceIntRegion());
}

void QtCompositorWidget::EndRemoteDrawingInRegion(
    gfx::DrawTarget* aDrawTarget,
    const LayoutDeviceIntRegion& aInvalidRegion) {
  if (!mWidget || !mBuffers[mDrawIndex]) {
    return;
  }
  if (aDrawTarget) {
    aDrawTarget->Flush();
  }

  const LayoutDeviceIntSize size = mBufferSize;
  const LayoutDeviceIntRegion full(
      LayoutDeviceIntRect(0, 0, size.width, size.height));
  LayoutDeviceIntRegion damage = aInvalidRegion.IsEmpty()
                                     ? full
                                     : aInvalidRegion.Intersect(full);

  RefPtr<gfx::DataSourceSurface> toPresent = mBuffers[mDrawIndex];
  int presentIndex = mDrawIndex;

  // If this buffer is still busy on the main thread, take a snapshot copy so
  // we never race with PresentImage / QImage lifetime.
  if (mBufferBusyBits & (1u << uint32_t(presentIndex))) {
    gfx::DataSourceSurface::ScopedMap srcMap(toPresent,
                                             gfx::DataSourceSurface::READ);
    if (!srcMap.IsMapped()) {
      return;
    }
    RefPtr<gfx::DataSourceSurface> snapshot =
        gfx::Factory::CreateDataSourceSurface(
            gfx::IntSize(size.width, size.height),
            gfx::SurfaceFormat::B8G8R8A8, /* aZero = */ false);
    if (!snapshot) {
      return;
    }
    gfx::DataSourceSurface::ScopedMap dstMap(snapshot,
                                             gfx::DataSourceSurface::WRITE);
    if (!dstMap.IsMapped()) {
      return;
    }
    const int32_t srcStride = srcMap.GetStride();
    const int32_t dstStride = dstMap.GetStride();
    const size_t rowBytes = size_t(size.width) * 4;
    if (srcStride == dstStride && size_t(srcStride) == rowBytes) {
      memcpy(dstMap.GetData(), srcMap.GetData(),
             size_t(srcStride) * size_t(size.height));
    } else {
      for (int32_t y = 0; y < size.height; ++y) {
        memcpy(dstMap.GetData() + y * dstStride,
               srcMap.GetData() + y * srcStride, rowBytes);
      }
    }
    toPresent = std::move(snapshot);
    presentIndex = -1;  // heap snapshot, not a ring buffer slot
  }

  {
    auto pending = mPendingPresent.Lock();
    // Release busy bit for a coalesced-away ring buffer.
    if (pending->mBufferIndex >= 0) {
      mBufferBusyBits &= ~(1u << uint32_t(pending->mBufferIndex));
    }
    if (pending->mSurface) {
      damage.OrWith(pending->mDamage);
    }
    if (presentIndex >= 0) {
      mBufferBusyBits |= (1u << uint32_t(presentIndex));
    }
    pending->mSurface = std::move(toPresent);
    pending->mDamage = std::move(damage);
    pending->mBufferIndex = presentIndex;
  }

  // Advance draw index for the next frame.
  mDrawIndex = (mDrawIndex + 1) % kBufferCount;

  if (mPresentPosted.exchange(true)) {
    return;
  }

  RefPtr<QtCompositorWidget> self = this;
  nsCOMPtr<nsIRunnable> task = NS_NewRunnableFunction(
      "QtCompositorWidget::PresentSoftwareBuffer", [self]() {
        self->mPresentPosted = false;
        RefPtr<gfx::DataSourceSurface> frame;
        LayoutDeviceIntRegion damage;
        int bufferIndex = -1;
        {
          auto pending = self->mPendingPresent.Lock();
          frame = pending->mSurface.forget();
          damage = std::move(pending->mDamage);
          bufferIndex = pending->mBufferIndex;
          pending->mBufferIndex = -1;
          pending->mDamage.SetEmpty();
        }
        if (frame) {
          self->PresentSoftwareBuffer(frame, damage);
        }
        if (bufferIndex >= 0) {
          self->mBufferBusyBits &= ~(1u << uint32_t(bufferIndex));
        }
      });
  // Prefer render-blocking so scrolls are not starved by idle main-thread work.
  (void)NS_DispatchToMainThreadQueue(task.forget(),
                                     EventQueuePriority::RenderBlocking);
}

void QtCompositorWidget::PresentSoftwareBuffer(
    gfx::DataSourceSurface* aFrame, const LayoutDeviceIntRegion& aDamage) {
  if (!mWidget || !aFrame || mWidget->Destroyed()) {
    return;
  }
  // Exclusive-EGL windows normally present via SwapBuffers. A single SW
  // present (size race / basic-compositor probe) used to AbandonHardware
  // immediately and flip chrome to software mid-frame — urlbar blink.
  // Tolerate a few SW frames, then fall back so permanent SWGL still works.
  if (mWidget->UsesHardwarePresent()) {
    if (++mSwPresentWhileHardware < 10) {
      return;
    }
    mWidget->AbandonHardwarePresent();
    mSwPresentWhileHardware = 0;
  }
  QtWindowBridge* window = mWidget->GetQtWindow();
  if (!window) {
    return;
  }

  // Keep the surface (+ map) alive for as long as QImage references its
  // pixels — eliminates the extra full-frame QImage::copy() on every present.
  struct FrameHolder {
    RefPtr<gfx::DataSourceSurface> mSurface;
    gfx::DataSourceSurface::ScopedMap mMap;
    explicit FrameHolder(gfx::DataSourceSurface* aSurface)
        : mSurface(aSurface),
          mMap(aSurface, gfx::DataSourceSurface::READ) {}
  };
  auto* holder = new FrameHolder(aFrame);
  if (!holder->mMap.IsMapped()) {
    delete holder;
    return;
  }
  const gfx::IntSize size = aFrame->GetSize();
  QImage image(holder->mMap.GetData(), size.width, size.height,
               holder->mMap.GetStride(), QImage::Format_ARGB32_Premultiplied,
               [](void* p) { delete static_cast<FrameHolder*>(p); }, holder);

  // Convert layout-device damage to DIP window coords for QBackingStore.
  const double dpr = std::max(1.0, mWidget->DevicePixelRatio());
  QRegion damageDip;
  const LayoutDeviceIntRect bounds = aDamage.GetBounds();
  const bool fullFrame =
      aDamage.IsEmpty() ||
      (bounds.x <= 0 && bounds.y <= 0 && bounds.XMost() >= size.width &&
       bounds.YMost() >= size.height);
  if (!fullFrame) {
    for (auto iter = aDamage.RectIter(); !iter.Done(); iter.Next()) {
      const LayoutDeviceIntRect& r = iter.Get();
      damageDip += QRect(int(std::lround(r.x / dpr)),
                         int(std::lround(r.y / dpr)),
                         int(std::lround(r.width / dpr)),
                         int(std::lround(r.height / dpr)));
    }
  }
  window->PresentImage(std::move(image), damageDip);
}

LayoutDeviceIntSize QtCompositorWidget::GetClientSize() {
  auto size = mClientSize.Lock();
  return *size;
}

nsIWidget* QtCompositorWidget::RealWidget() { return mWidget; }

EGLNativeWindowType QtCompositorWidget::GetEGLNativeWindow() {
  // Live handle: X11 XID or Wayland wl_egl_window for exclusive HW windows.
  if (mWidget) {
    const uintptr_t live = mWidget->EnsureEGLNativeWindow();
    if (live) {
      mNativeWindowHandle = live;
    }
  }
  return reinterpret_cast<EGLNativeWindowType>(mNativeWindowHandle);
}

void QtCompositorWidget::SetEGLNativeWindowSize(
    const LayoutDeviceIntSize& aSize) {
  if (mWidget) {
    mWidget->ResizeWaylandEglWindow(aSize);
  }
  {
    auto size = mClientSize.Lock();
    if (aSize.width > 0 && aSize.height > 0) {
      *size = aSize;
    }
  }
}

void QtCompositorWidget::NotifyClientSizeChanged(
    const LayoutDeviceIntSize& aClientSize) {
  {
    auto size = mClientSize.Lock();
    *size = aClientSize;
  }
  // Keep Wayland wl_egl_window in sync for EGL window surfaces.
  if (mWidget) {
    mWidget->ResizeWaylandEglWindow(aClientSize);
  }
}

void QtCompositorWidget::NotifyFullscreenChanged(bool) {}

bool QtCompositorWidget::IsHidden() const {
  return !mWidget || mWidget->Destroyed() || !mWidget->IsVisible();
}

void QtCompositorWidget::CleanupResources() {
  for (int i = 0; i < kBufferCount; ++i) {
    mDrawTargets[i] = nullptr;
    mBuffers[i] = nullptr;
  }
  mBufferSize = LayoutDeviceIntSize();
  mBufferBusyBits = 0;
  {
    auto pending = mPendingPresent.Lock();
    pending->mSurface = nullptr;
    pending->mDamage.SetEmpty();
    pending->mBufferIndex = -1;
  }
  mPresentPosted = false;
  mSwPresentWhileHardware = 0;
  mNativeWindowHandle = 0;
  // Drop the widget ref so in-flight present runnables no-op after Destroy.
  mWidget = nullptr;
}

uintptr_t QtCompositorWidget::GetWidgetKey() {
  return reinterpret_cast<uintptr_t>(mWidget.get());
}

}  // namespace mozilla::widget
