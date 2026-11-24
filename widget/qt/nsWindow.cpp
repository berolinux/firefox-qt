/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsWindow.h"
#include "ScreenHelperQt.h"

#ifdef MOZ_ENABLE_DBUS
#  include "DBusMenuBar.h"
#endif
#include "QtWaylandVsyncSource.h"
#include "VsyncDispatcher.h"
#include "NativeKeyBindings.h"
#include "QtCompositorWidget.h"
#include "QtTextEventDispatcherListener.h"
#include "QtWindowBridge.h"
#include "WindowOcclusionState.h"
#include "nsDragService.h"
#include "nsUserIdleServiceQt.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "mozilla/EventForwards.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/PresShell.h"
#include "mozilla/TextEventDispatcher.h"
#include "mozilla/TextEvents.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/WidgetUtils.h"
#include "mozilla/WritingModes.h"
#include "mozilla/Maybe.h"
#include "mozilla/dom/WheelEventBinding.h"
#include "nsIDragService.h"
#include "nsIDragSession.h"
#include "nsIScreen.h"
#include "nsIScreenManager.h"
#include "nsIWidgetListener.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsReadableUtils.h"
#include "prenv.h"
#include "nsAppRunner.h"

// MOZ_QT_SOFTWARE_PRESENT=1 forces the QBackingStore path (escape hatch).
static bool QtHardwarePresentAllowed() {
  return !PR_GetEnv("MOZ_QT_SOFTWARE_PRESENT");
}

#ifdef ACCESSIBILITY
#  include "mozilla/a11y/LocalAccessible.h"
#  include "mozilla/a11y/Platform.h"
#  include "nsAccessibilityService.h"
#  include "nsIAccessibleEvent.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QCursor>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QFont>
#include <QMouseEvent>
#include <QPointer>
#include <QRegion>
#include <QScreen>
#include <QSize>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QWheelEvent>
#include <QWindow>
#include <QWindowStateChangeEvent>
#include <QLocale>
#include <qpa/qplatformnativeinterface.h>
#include <QtGui/qguiapplication_platform.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#ifdef MOZ_X11
#  include <X11/Xatom.h>
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#  include <X11/extensions/shape.h>
#endif

using namespace mozilla;
using namespace mozilla::widget;

/* static */
bool nsWindow::IsWaylandPlatform() {
  return QGuiApplication::platformName().contains(QLatin1String("wayland"),
                                                  Qt::CaseInsensitive);
}

nsWindow::nsWindow() = default;

nsWindow::~nsWindow() { Destroy(); }

void nsWindow::ReleaseGlobals() {
  NativeKeyBindings::Shutdown();
  QtTextEventDispatcherListener::Shutdown();
}

nsWindow* nsWindow::FromWidget(nsIWidget* aWidget) {
  return static_cast<nsWindow*>(aWidget);
}

double nsWindow::DevicePixelRatio() const {
  // Popups often report devicePixelRatio()==1 until first mapped. Gecko converts
  // screen (device) bounds through this widget's desktop scale when calling
  // Move/Resize; if the popup's DPR disagrees with the parent, menus land at
  // the wrong place on Wayland. Always inherit the transient parent's scale.
  if (mTransientParentWidget) {
    return mTransientParentWidget->DevicePixelRatio();
  }
  if (mQtWindow) {
    const qreal dpr = mQtWindow->devicePixelRatio();
    if (dpr > 0.0) {
      return double(dpr);
    }
    if (QWindow* parent = mQtWindow->transientParent()) {
      const qreal pdpr = parent->devicePixelRatio();
      if (pdpr > 0.0) {
        return double(pdpr);
      }
      if (parent->screen()) {
        const qreal sdpr = parent->screen()->devicePixelRatio();
        if (sdpr > 0.0) {
          return double(sdpr);
        }
      }
    }
    if (mQtWindow->screen()) {
      const qreal sdpr = mQtWindow->screen()->devicePixelRatio();
      if (sdpr > 0.0) {
        return double(sdpr);
      }
    }
  }
  if (QScreen* screen = QGuiApplication::primaryScreen()) {
    const qreal dpr = screen->devicePixelRatio();
    if (dpr > 0.0) {
      return double(dpr);
    }
  }
  return 1.0;
}

LayoutDeviceIntPoint nsWindow::QtDipToDevice(const QPointF& aDip) const {
  const double dpr = DevicePixelRatio();
  return LayoutDeviceIntPoint(int32_t(std::lround(aDip.x() * dpr)),
                              int32_t(std::lround(aDip.y() * dpr)));
}

void nsWindow::ConfigureWaylandPopupPlacement(const QSize& aSizeDip) {
  // mBounds is Gecko's global layout-device position (CalcWidgetBounds /
  // WidgetToScreenOffset space). xdg_popup geometry is parent-relative.
  // Convert using the *parent* DPR only (see DevicePixelRatio()).
  const double dpr = DevicePixelRatio();
  auto toDip = [dpr](int32_t devicePx) {
    return int(std::lround(double(devicePx) / dpr));
  };

  int localX = toDip(mBounds.x);
  int localY = toDip(mBounds.y);

  if (mTransientParentWidget) {
    if (mTransientParentWidget->mQtWindow &&
        mTransientParentWidget->mQtWindow->screen()) {
      mQtWindow->setScreen(mTransientParentWidget->mQtWindow->screen());
    }
    const LayoutDeviceIntPoint parentOrigin =
        mTransientParentWidget->WidgetToScreenOffset();
    localX = toDip(mBounds.x - parentOrigin.x);
    localY = toDip(mBounds.y - parentOrigin.y);
  } else if (QWindow* parent = mQtWindow->transientParent()) {
    if (parent->screen()) {
      mQtWindow->setScreen(parent->screen());
    }
    const QPoint parentGlobal = parent->mapToGlobal(QPoint(0, 0));
    localX = toDip(mBounds.x) - parentGlobal.x();
    localY = toDip(mBounds.y) - parentGlobal.y();
  }

  mQtWindow->setGeometry(localX, localY, aSizeDip.width(), aSizeDip.height());
}

void nsWindow::ApplyQtGeometryFromBounds() {
  if (!mQtWindow) {
    return;
  }
  // For popups, DevicePixelRatio() follows the transient parent so Gecko's
  // desktop↔device conversion matches the parent window.
  const double dpr = DevicePixelRatio();
  const int w = std::max(1, int(std::lround(mBounds.width / dpr)));
  const int h = std::max(1, int(std::lround(mBounds.height / dpr)));

  if (IsPopup() && IsWaylandPlatform()) {
    ConfigureWaylandPopupPlacement(QSize(w, h));
    return;
  }

  const int x = int(std::lround(mBounds.x / dpr));
  const int y = int(std::lround(mBounds.y / dpr));
  mQtWindow->setGeometry(x, y, w, h);
}

nsresult nsWindow::Create(nsIWidget* aParent, const LayoutDeviceIntRect& aRect,
                          const InitData& aInitData) {
  BaseCreate(aParent, aInitData);

  mBounds = aRect;
  mTransparencyMode = aInitData.mTransparencyMode;
  mAlwaysOnTop = aInitData.mAlwaysOnTop;
  mIsAlert = aInitData.mIsAlert;
  mIsDragPopup = aInitData.mIsDragPopup;
  mResizable = aInitData.mResizable;
  mIsAnimationSuppressed = aInitData.mIsAnimationSuppressed;
  // Exclusive EGL for opaque non-popups. Popups/transparent keep SW present
  // (rounded alpha panels need QBackingStore). Media PiP is short-lived and
  // streams YUV; SW present avoids SWGL CompositeYUV UAF on unpip when the
  // exclusive-EGL path races compositor teardown. Escape: MOZ_QT_SOFTWARE_PRESENT=1.
  mUseHardwarePresent =
      QtHardwarePresentAllowed() &&
      aInitData.mWindowType != WindowType::Popup && !mIsDragPopup &&
      aInitData.mTransparencyMode == TransparencyMode::Opaque &&
      mPiPType != PiPType::MediaPiP;

  mQtWindow = mozilla::MakeUnique<QtWindowBridge>(this);
  mQtWindow->setTitle(QStringLiteral("Mozilla Firefox"));
  // Inherit application icon (theme / desktop file) for taskbar / Alt-Tab.
  if (!QGuiApplication::windowIcon().isNull()) {
    mQtWindow->setIcon(QGuiApplication::windowIcon());
  }
  if (mUseHardwarePresent) {
    mQtWindow->ConfigureForHardwarePresent();
  }

  // Window type / flags before geometry so the QPA backend creates the right
  // role (xdg_popup vs xdg_toplevel) on Wayland.
  if (aInitData.mWindowType == WindowType::Popup) {
    // Frameless + no system drop shadow: chrome draws its own rounded panel
    // and shadow; a WM frame shows as a black rectangle around the menu.
    Qt::WindowFlags flags = Qt::Popup | Qt::FramelessWindowHint |
                            Qt::NoDropShadowWindowHint;
    // Doorhangers / tooltips must not steal keyboard focus from the browser.
    // PopupNotifications rejects Allow/Deny when the chrome window is inactive
    // (Services.focus.activeWindow / :-moz-window-inactive).
    if (aInitData.mPopupHint == PopupType::Panel ||
        aInitData.mPopupHint == PopupType::Tooltip) {
      flags |= Qt::WindowDoesNotAcceptFocus;
    }
    // Drag feedback: stay above, no activation (matches GDK_WINDOW_TYPE_HINT_DND).
    if (mIsDragPopup) {
      flags |= Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus |
               Qt::WindowTransparentForInput;
    }
    // PopupLevel::Top: keep above other apps when possible (Parent stacks with
    // the owner via transient parent below).
    if (aInitData.mPopupLevel == PopupLevel::Top) {
      flags |= Qt::WindowStaysOnTopHint;
    }
    mQtWindow->setFlags(flags);
  } else if (aInitData.mWindowType == WindowType::Dialog) {
    Qt::WindowFlags flags = Qt::Dialog;
    // Media Picture-in-Picture: utility-style always-on-top chrome window.
    if (mPiPType == PiPType::MediaPiP || mAlwaysOnTop) {
      flags |= Qt::WindowStaysOnTopHint | Qt::Tool;
      // Frameless matches the PiP player chrome (titlebar=no on non-macOS).
      if (mPiPType == PiPType::MediaPiP) {
        flags |= Qt::FramelessWindowHint;
      }
    }
    // System alerts / notifications: on top, tool-style, no focus steal.
    if (mIsAlert) {
      flags |= Qt::WindowStaysOnTopHint | Qt::Tool |
               Qt::WindowDoesNotAcceptFocus | Qt::FramelessWindowHint;
    }
    mQtWindow->setFlags(flags);
  } else {
    Qt::WindowFlags flags = Qt::Window;
    if (mAlwaysOnTop || mIsAlert) {
      flags |= Qt::WindowStaysOnTopHint;
    }
    if (mIsAlert) {
      flags |= Qt::Tool | Qt::WindowDoesNotAcceptFocus;
    }
    mQtWindow->setFlags(flags);
  }

  // Per-pixel alpha only when Gecko requests transparency (rounded menus /
  // arrow panels). Forcing alpha on *all* popups left opaque tooltips as
  // empty white surfaces over video until the first SW present arrived.
  if (mTransparencyMode != TransparencyMode::Opaque) {
    QSurfaceFormat fmt = mQtWindow->format();
    fmt.setAlphaBufferSize(8);
    mQtWindow->setFormat(fmt);
    mQtWindow->SetTransparentBackground(true);
  }

  if (aParent) {
    nsWindow* parent = FromWidget(aParent);
    if (parent && parent->mQtWindow) {
      // Use transient parent for WM stacking / xdg_popup parenting only.
      // Do NOT QObject-parent the QWindow: ownership would delete this
      // window when the parent is destroyed while we still hold UniquePtr.
      // PopupLevel::Top still needs a parent on Wayland for xdg_popup.
      mTransientParentWidget = parent;
      mQtWindow->setTransientParent(parent->mQtWindow.get());
      if (parent->mQtWindow->screen()) {
        mQtWindow->setScreen(parent->mQtWindow->screen());
      }
    }
  }

  // Map Gecko BorderStyle / resizable chrome flags onto Qt decorations.
  if (aInitData.mWindowType != WindowType::Popup) {
    ApplyWindowDecoration(mBorderStyle);
  }

  // aRect is layout-device pixels; QWindow geometry is device-independent.
  ApplyQtGeometryFromBounds();

  // Create the platform window early so EnsureEGLNativeWindow can bind.
  if (mUseHardwarePresent && mQtWindow) {
    mQtWindow->create();
    (void)EnsureEGLNativeWindow();
  }

  ApplyKioskModeIfNeeded();
  MaybeInitWaylandVsync();

  return NS_OK;
}

void nsWindow::MaybeInitWaylandVsync() {
  // Disabled: empty wl_surface_commit from frame-callback vsync races Qt's
  // surface ownership and prevents the window from mapping. Use the global
  // display-rate software vsync source until we can drive callbacks without
  // committing on Qt-managed surfaces.
  (void)0;
}

void nsWindow::CreateCompositorVsyncDispatcher() {
  if (!mWaylandVsyncSource) {
    nsIWidget::CreateCompositorVsyncDispatcher();
    return;
  }
  if (!mCompositorVsyncDispatcherLock) {
    mCompositorVsyncDispatcherLock =
        MakeUnique<Mutex>("mCompositorVsyncDispatcherLock");
  }
  MutexAutoLock lock(*mCompositorVsyncDispatcherLock);
  if (!mCompositorVsyncDispatcher) {
    mCompositorVsyncDispatcher =
        new CompositorVsyncDispatcher(mWaylandVsyncDispatcher);
  }
}

RefPtr<VsyncDispatcher> nsWindow::GetVsyncDispatcher() {
  return mWaylandVsyncDispatcher;
}

void nsWindow::NotifyOcclusionState(OcclusionState aState) {
  if (!IsTopLevelWidget()) {
    return;
  }
  const bool isFullyOccluded = aState == OcclusionState::OCCLUDED;
  if (mIsFullyOccluded == isFullyOccluded) {
    return;
  }
  mIsFullyOccluded = isFullyOccluded;
  if (mWidgetListener) {
    mWidgetListener->OcclusionStateChanged(mIsFullyOccluded);
  }
}

void nsWindow::Destroy() {
  if (mIsDestroyed) {
    return;
  }
  mIsDestroyed = true;

#ifdef ACCESSIBILITY
  mRootAccessible = nullptr;
#endif

  nsCOMPtr<nsIWidget> kungFuDeathGrip(this);

  // Stop vsync sources first so no further frames are requested.
  if (mWaylandVsyncSource) {
    mWaylandVsyncSource->Shutdown();
    mWaylandVsyncSource = nullptr;
  }
  mWaylandVsyncDispatcher = nullptr;

  // Fully shut down WebRender / SWGL (including SwCompositeThread YUV jobs)
  // *before* clearing presentation buffers or destroying the native window.
  // Cleaning up the compositor widget first raced CompositeYUV against freed
  // video textures when closing Picture-in-Picture (return to tab).
  DestroyLayerManager();

  if (mCompositorWidgetDelegate) {
    mCompositorWidgetDelegate->CleanupResources();
    mCompositorWidgetDelegate = nullptr;
  }

  // Parent/child unlink. DestroyCompositor is a no-op if already torn down.
  nsIWidget::Destroy();

  DestroyWaylandEglWindow();

  mTransientParentWidget = nullptr;
  if (mQtWindow) {
    mQtWindow->ClearWidget();
    // Ensure we own the QWindow exclusively for the rest of teardown.
    mQtWindow->setTransientParent(nullptr);
    mQtWindow->setParent(nullptr);
    if (mQtWindow->isVisible()) {
      mQtWindow->hide();
    }
    // Destroy platform resources then drop UniquePtr (deletes QWindow).
    // Avoid re-entrancy into Gecko from closeEvent during shutdown.
    mQtWindow->destroy();
    mQtWindow = nullptr;
  }

  if (!mOnDestroyCalled) {
    mOnDestroyCalled = true;
    OnDestroy();
    NotifyWindowDestroyed();
  }
}

void nsWindow::Show(bool aState) {
  if (!mQtWindow || mIsDestroyed) {
    return;
  }
  mIsVisible = aState;
  if (aState) {
    // Re-apply geometry immediately before show so Wayland xdg_popup gets the
    // final parent-local position and full size after Gecko's last Move/Resize.
    if (IsPopup()) {
      if (mGeckoPopupSize.width > 0 && mGeckoPopupSize.height > 0) {
        mBounds.SizeTo(mGeckoPopupSize);
      }
      ApplyQtGeometryFromBounds();
    }
    // Always push the current client size to the compositor before mapping.
    // SWGL fails with "no dt" when InitialClientSize was still 0x0.
    if (mCompositorWidgetDelegate && mBounds.width > 0 && mBounds.height > 0) {
      mCompositorWidgetDelegate->NotifyClientSizeChanged(mBounds.Size());
    }
    if (gKioskMode && mWindowType == WindowType::TopLevel) {
      mSizeMode = nsSizeMode_Fullscreen;
      mQtWindow->showFullScreen();
    } else {
      mQtWindow->show();
    }
    // Wayland often only exposes wl_surface after map; refresh EGL native.
    if (mUseHardwarePresent) {
      (void)EnsureEGLNativeWindow();
    }
    // Match GTK/Windows: always-on-top Media PiP / alerts must not steal
    // activation. If they do, the originating tab goes inactive, media can
    // pause, and PiP videocontrols stay on the main page.
    // Also: Qt::Popup panels with WindowDoesNotAcceptFocus (doorhangers,
    // tooltips, many TB chrome menus) still cause the parent to lose focus on
    // KDE/Wayland. Without re-activating the parent, chrome goes inactive
    // (:-moz-window-inactive) and the surface is restacked behind other apps.
    const bool isNoFocusPopup =
        IsPopup() && (mQtWindow->flags() & Qt::WindowDoesNotAcceptFocus);
    const bool noStealFocus =
        mIsAlert || isNoFocusPopup ||
        (mAlwaysOnTop && mPiPType != PiPType::DocumentPiP);
    if (noStealFocus && mTransientParentWidget) {
      // Single parent reclaim only — no retries, no suppress timer.
      mTransientParentWidget->ReclaimActivation();
      mTransientParentWidget->ScheduleReclaimActivation();
    } else if (!IsPopup() && !mIsAlert && ShouldAcceptActivate()) {
      // Dialog/top-level: raise this window only (not parent). Parent raise
      // while a dialog maps fights focus and freezes input on KDE/Wayland.
      ReclaimActivation();
      ScheduleReclaimActivation();
    }
#ifdef ACCESSIBILITY
    if (a11y::ShouldA11yBeEnabled()) {
      CreateRootAccessible();
    }
#endif
  } else {
    if (mWaylandVsyncSource) {
      mWaylandVsyncSource->DisableVSyncSource();
    }
    // After popup/dialog unmap, re-activate parent once (deferred).
    const bool isNoFocusPopup =
        IsPopup() && mQtWindow &&
        (mQtWindow->flags() & Qt::WindowDoesNotAcceptFocus);
    const bool reclaimParentOnHide =
        mTransientParentWidget &&
        (isNoFocusPopup || mIsAlert || mWindowType == WindowType::Dialog);
    mQtWindow->hide();
    if (reclaimParentOnHide) {
      mTransientParentWidget->ScheduleReclaimActivation();
    }
  }
}

bool nsWindow::IsVisible() const { return mIsVisible; }

void nsWindow::Move(const DesktopPoint& aPoint) {
  if (!mQtWindow) {
    return;
  }
  DesktopToLayoutDeviceScale scale = GetDesktopToDeviceScale();
  LayoutDeviceIntPoint pos = RoundedToInt(aPoint * scale);
  mBounds.MoveTo(pos);
  if (IsPopup() && IsWaylandPlatform()) {
    // Popups need full geometry apply (parent-local conversion + size).
    ApplyQtGeometryFromBounds();
  } else {
    // Toplevel Move must NOT call setGeometry with size — that re-applied a
    // stale mBounds size and reintroduced the "canvas too large" bug.
    const double dpr = DevicePixelRatio();
    mQtWindow->setX(int(std::lround(pos.x / dpr)));
    mQtWindow->setY(int(std::lround(pos.y / dpr)));
  }
}

void nsWindow::Resize(const DesktopSize& aSize, bool aRepaint) {
  DesktopToLayoutDeviceScale scale = GetDesktopToDeviceScale();
  Resize(DesktopRect(mBounds.X() / scale.scale, mBounds.Y() / scale.scale,
                     aSize.width, aSize.height),
         aRepaint);
}

void nsWindow::Resize(const DesktopRect& aRect, bool aRepaint) {
  if (!mQtWindow) {
    return;
  }
  DesktopToLayoutDeviceScale scale = GetDesktopToDeviceScale();
  mBounds = RoundedToInt(aRect * scale);
  // Honor chrome "lockaspectratio" (PiP): match GTK/Windows behaviour.
  if (mAspectRatio > 0.0 && mBounds.width > 0 && mBounds.height > 0) {
    const double w = mBounds.width;
    const double h = mBounds.height;
    const double current = w / h;
    if (current > mAspectRatio) {
      mBounds.width = int32_t(std::lround(h * mAspectRatio));
    } else if (current < mAspectRatio) {
      mBounds.height = int32_t(std::lround(w / mAspectRatio));
    }
  }
  if (IsPopup()) {
    mGeckoPopupSize = mBounds.Size();
  }
  ApplyQtGeometryFromBounds();
  // Compositor must track Gecko's size even if the platform configure lags.
  if (mCompositorWidgetDelegate) {
    mCompositorWidgetDelegate->NotifyClientSizeChanged(mBounds.Size());
  }
  if (aRepaint) {
    mQtWindow->requestUpdate();
  }
  DispatchResizeEvent();
}

void nsWindow::Enable(bool aState) {
  mEnabled = aState;
  if (mQtWindow) {
    mQtWindow->setFlags(mQtWindow->flags() |
                        (aState ? Qt::WindowFlags() : Qt::WindowTransparentForInput));
  }
}

bool nsWindow::IsEnabled() const { return mEnabled; }

void nsWindow::SetFocus(Raise aRaise, mozilla::dom::CallerType) {
  if (!mQtWindow) {
    return;
  }
  // Media PiP / alerts are always-on-top but must not pull activation off the
  // owning tab (GTK focus_on_map=false / Win SW_SHOWNOACTIVATE).
  const bool allowActivate =
      (!mAlwaysOnTop || mPiPType == PiPType::DocumentPiP) && !mIsAlert;
  if (aRaise == Raise::Yes) {
    mQtWindow->raise();
  }
  if (allowActivate) {
    mQtWindow->requestActivate();
  }
}

LayoutDeviceIntRect nsWindow::GetBounds() { return mBounds; }

LayoutDeviceIntRect nsWindow::GetClientBounds() {
  // No OS chrome insets yet; client area matches the QWindow surface.
  return mBounds;
}

LayoutDeviceIntPoint nsWindow::GetClientOffset() {
  // We draw into the full QWindow surface (no separate client shift).
  return LayoutDeviceIntPoint(0, 0);
}

void nsWindow::SetSizeMode(nsSizeMode aMode) {
  if (!mQtWindow || mSizeMode == aMode) {
    return;
  }
  // Remember pre-fullscreen mode so MakeFullScreen(false) / WM unfullscreen
  // can restore maximized vs normal correctly.
  if (aMode == nsSizeMode_Fullscreen && mSizeMode != nsSizeMode_Minimized) {
    mLastSizeModeBeforeFullscreen = mSizeMode;
  }
  mSizeMode = aMode;
  switch (aMode) {
    case nsSizeMode_Minimized:
      mQtWindow->showMinimized();
      break;
    case nsSizeMode_Maximized:
      mQtWindow->showMaximized();
      break;
    case nsSizeMode_Fullscreen:
      // Must use the platform fullscreen protocol (xdg_toplevel on Wayland).
      // setGeometry() to the screen size alone is ignored by the compositor.
      mQtWindow->showFullScreen();
      break;
    case nsSizeMode_Normal:
    default:
      mQtWindow->showNormal();
      break;
  }
  if (mWidgetListener) {
    mWidgetListener->SizeModeChanged(mSizeMode);
  }
}

nsSizeMode nsWindow::SizeMode() { return mSizeMode; }

nsresult nsWindow::MakeFullScreen(bool aFullScreen) {
  if (!mQtWindow || mIsDestroyed) {
    return NS_ERROR_FAILURE;
  }

  // Kiosk mode always stays fullscreen (matches GTK).
  if (!aFullScreen && gKioskMode) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // Do not use InfallibleMakeFullScreen's setGeometry path: on Wayland/KWin
  // (and often X11) free-form resize to cover the screen is rejected, so the
  // window stays windowed, content layout fights the real size, and the UI
  // flickers. Always request true window-manager fullscreen.
  if (aFullScreen) {
    if (mSizeMode != nsSizeMode_Fullscreen &&
        mSizeMode != nsSizeMode_Minimized) {
      mLastSizeModeBeforeFullscreen = mSizeMode;
    }
    // Media PiP: unlock aspect ratio while fullscreen (restored on exit).
    if (mPiPType == PiPType::MediaPiP && mAspectRatio > 0.0) {
      mAspectRatioSaved = mAspectRatio;
      mAspectRatio = 0.0;
    }
    mSizeMode = nsSizeMode_Fullscreen;
    mQtWindow->showFullScreen();
  } else {
    if (mPiPType == PiPType::MediaPiP && mAspectRatioSaved > 0.0) {
      mAspectRatio = mAspectRatioSaved;
      mAspectRatioSaved = 0.0;
    }
    const nsSizeMode restore = mLastSizeModeBeforeFullscreen;
    mSizeMode =
        (restore == nsSizeMode_Maximized) ? nsSizeMode_Maximized : nsSizeMode_Normal;
    if (mSizeMode == nsSizeMode_Maximized) {
      mQtWindow->showMaximized();
    } else {
      mQtWindow->showNormal();
    }
  }

  if (mWidgetListener) {
    mWidgetListener->SizeModeChanged(mSizeMode);
  }
  // Resize/compositor update follows via QtWindowBridge::resizeEvent once the
  // compositor applies the fullscreen configure.
  return NS_OK;
}

nsresult nsWindow::SetTitle(const nsAString& aTitle) {
  if (mQtWindow) {
    mQtWindow->setTitle(QString::fromUtf16(
        reinterpret_cast<const char16_t*>(aTitle.BeginReading()),
        int(aTitle.Length())));
  }
  return NS_OK;
}

void nsWindow::SetIcon(const nsAString& aIconSpec) {
  if (!mQtWindow) {
    return;
  }
  // aIconSpec is typically a resource name without extension; try common paths.
  QString base = QString::fromUtf16(
      reinterpret_cast<const char16_t*>(aIconSpec.BeginReading()),
      int(aIconSpec.Length()));
  QIcon icon;
  if (!base.isEmpty()) {
    icon = QIcon::fromTheme(base);
    if (icon.isNull()) {
      icon.addFile(base + QStringLiteral(".png"));
    }
  }
  // Fall back to the application icon so Plasma keeps the Firefox icon
  // when chrome passes a missing resource name.
  if (icon.isNull()) {
    icon = QGuiApplication::windowIcon();
  }
  if (!icon.isNull()) {
    mQtWindow->setIcon(icon);
  }
}

static Qt::CursorShape QtCursorShape(nsCursor aCursor) {
  switch (aCursor) {
    case eCursor_wait:
      return Qt::WaitCursor;
    case eCursor_select:
      return Qt::IBeamCursor;
    case eCursor_hyperlink:
      return Qt::PointingHandCursor;
    case eCursor_n_resize:
      return Qt::SizeVerCursor;
    case eCursor_s_resize:
      return Qt::SizeVerCursor;
    case eCursor_w_resize:
      return Qt::SizeHorCursor;
    case eCursor_e_resize:
      return Qt::SizeHorCursor;
    case eCursor_nw_resize:
    case eCursor_se_resize:
    case eCursor_nwse_resize:
      return Qt::SizeFDiagCursor;
    case eCursor_ne_resize:
    case eCursor_sw_resize:
    case eCursor_nesw_resize:
      return Qt::SizeBDiagCursor;
    case eCursor_ns_resize:
      return Qt::SizeVerCursor;
    case eCursor_ew_resize:
    case eCursor_col_resize:
      return Qt::SizeHorCursor;
    case eCursor_row_resize:
      return Qt::SizeVerCursor;
    case eCursor_crosshair:
    case eCursor_cell:
      return Qt::CrossCursor;
    case eCursor_move:
    case eCursor_all_scroll:
    case eCursor_grabbing:
      return Qt::SizeAllCursor;
    case eCursor_grab:
      return Qt::OpenHandCursor;
    case eCursor_help:
      return Qt::WhatsThisCursor;
    case eCursor_copy:
      return Qt::DragCopyCursor;
    case eCursor_alias:
      return Qt::DragLinkCursor;
    case eCursor_not_allowed:
    case eCursor_no_drop:
      return Qt::ForbiddenCursor;
    case eCursor_spinning:
      return Qt::BusyCursor;
    case eCursor_zoom_in:
      return Qt::CursorShape(Qt::SizeVerCursor);  // no dedicated shape
    case eCursor_zoom_out:
      return Qt::CursorShape(Qt::SizeVerCursor);
    case eCursor_vertical_text:
      return Qt::IBeamCursor;
    case eCursor_none:
      return Qt::BlankCursor;
    case eCursor_context_menu:
    case eCursor_standard:
    default:
      return Qt::ArrowCursor;
  }
}

void nsWindow::SetCursor(const Cursor& aCursor) {
  if (!mUpdateCursor && mCursor == aCursor) {
    return;
  }
  nsIWidget::SetCursor(aCursor);
  mUpdateCursor = false;
  if (!mQtWindow || mWidgetCursorLocked) {
    return;
  }
  // Custom image cursors are not wired yet; use the default system shape.
  mQtWindow->setCursor(QCursor(QtCursorShape(aCursor.mDefaultCursor)));
}

void nsWindow::HideWindowChrome(bool aShouldHide) {
  if (!mQtWindow || mWindowChromeHidden == aShouldHide) {
    return;
  }
  mWindowChromeHidden = aShouldHide;
  ApplyWindowDecoration(aShouldHide ? BorderStyle::None : mBorderStyle);
}

bool nsWindow::ShouldAcceptActivate() const {
  if (IsPopup() || mIsAlert) {
    return false;
  }
  // Media PiP / always-on-top utility windows must not steal tab activation.
  if (mAlwaysOnTop && mPiPType != PiPType::DocumentPiP) {
    return false;
  }
  if (mPiPType == PiPType::MediaPiP) {
    return false;
  }
  return true;
}

void nsWindow::ReclaimActivation() {
  if (!mQtWindow || mIsDestroyed || !mIsVisible || !ShouldAcceptActivate()) {
    return;
  }
  mQtWindow->raise();
  mQtWindow->requestActivate();
}

void nsWindow::ScheduleReclaimActivation() {
  if (!mQtWindow || mIsDestroyed || !ShouldAcceptActivate()) {
    return;
  }
  QPointer<QtWindowBridge> weak = mQtWindow.get();
  QTimer::singleShot(0, mQtWindow.get(), [weak]() {
    if (!weak) {
      return;
    }
    if (nsWindow* w = weak->GetWidget()) {
      w->ReclaimActivation();
    }
  });
}

bool nsWindow::HasVisibleTransientChild(const nsWindow* aOther) const {
  return aOther && aOther->mTransientParentWidget == this && aOther->mIsVisible;
}

bool nsWindow::HasAnyVisibleTransientChild() const {
  for (QWindow* win : QGuiApplication::allWindows()) {
    auto* bridge = qobject_cast<QtWindowBridge*>(win);
    if (!bridge || !win->isVisible()) {
      continue;
    }
    if (nsWindow* w = bridge->GetWidget()) {
      if (HasVisibleTransientChild(w)) {
        return true;
      }
    }
  }
  return false;
}

void nsWindow::ApplyWindowDecoration(BorderStyle aStyle) {
  if (!mQtWindow || mWindowType == WindowType::Popup) {
    return;
  }

  // Preserve role bits (Window / Dialog / Tool / Popup) and stacking hints.
  Qt::WindowFlags flags = mQtWindow->flags();
  const bool wasVisible = mQtWindow->isVisible();

  const bool forceFrameless =
      mWindowChromeHidden || mIsAlert || mPiPType == PiPType::MediaPiP ||
      aStyle == BorderStyle::None;

  const bool wantFrame =
      !forceFrameless &&
      (aStyle == BorderStyle::Default || aStyle == BorderStyle::All ||
       !!(aStyle & (BorderStyle::Title | BorderStyle::Border |
                    BorderStyle::ResizeH | BorderStyle::Menu |
                    BorderStyle::Minimize | BorderStyle::Maximize |
                    BorderStyle::Close)));

  if (wantFrame) {
    flags &= ~Qt::FramelessWindowHint;
  } else {
    flags |= Qt::FramelessWindowHint;
  }

  // Non-resizable dialogs / fixed chrome: fixed-size hint + clamp min/max.
  const bool resizable =
      mResizable || aStyle == BorderStyle::Default ||
      aStyle == BorderStyle::All || !!(aStyle & BorderStyle::ResizeH);
  if (!resizable && mWindowType == WindowType::Dialog) {
    flags |= Qt::MSWindowsFixedSizeDialogHint;
  } else {
    flags &= ~Qt::MSWindowsFixedSizeDialogHint;
  }

  mQtWindow->setFlags(flags);

  if (!resizable && mBounds.width > 0 && mBounds.height > 0) {
    const double dpr = DevicePixelRatio();
    const int w = std::max(1, int(std::lround(mBounds.width / dpr)));
    const int h = std::max(1, int(std::lround(mBounds.height / dpr)));
    mQtWindow->setMinimumSize(QSize(w, h));
    mQtWindow->setMaximumSize(QSize(w, h));
  } else if (resizable) {
    // Restore unconstrained max when decorations allow resize; Gecko may
    // re-apply SetSizeConstraints afterward.
    constexpr int kUnconstrained = 16777215;
    if (mSizeConstraints.mMinSize.width > 0 ||
        mSizeConstraints.mMinSize.height > 0) {
      SetSizeConstraints(mSizeConstraints);
    } else {
      mQtWindow->setMinimumSize(QSize(0, 0));
      mQtWindow->setMaximumSize(QSize(kUnconstrained, kUnconstrained));
    }
  }

  if (wasVisible) {
    mQtWindow->show();
  }
}

void nsWindow::ApplyKioskModeIfNeeded() {
  if (!gKioskMode || mWindowType != WindowType::TopLevel || !mQtWindow) {
    return;
  }
  // Match GTK: kiosk always uses fullscreen. Defer the actual show to Show().
  mSizeMode = nsSizeMode_Fullscreen;
}

void nsWindow::SetModal(bool aModal) {
  mIsModal = aModal;
  if (!mQtWindow) {
    return;
  }
  mQtWindow->setModality(aModal ? Qt::ApplicationModal : Qt::NonModal);
}

void nsWindow::LockAspectRatio(bool aShouldLock) {
  if (aShouldLock && mBounds.width > 0 && mBounds.height > 0) {
    mAspectRatio = double(mBounds.width) / double(mBounds.height);
  } else {
    mAspectRatio = 0.0;
  }
}

void nsWindow::SetSizeConstraints(const SizeConstraints& aConstraints) {
  mSizeConstraints = aConstraints;
  if (!mQtWindow) {
    return;
  }
  const double dpr = DevicePixelRatio();
  auto toDip = [dpr](int32_t devicePx) -> int {
    return std::max(0, int(std::lround(devicePx / dpr)));
  };
  mQtWindow->setMinimumSize(
      QSize(toDip(aConstraints.mMinSize.width),
            toDip(aConstraints.mMinSize.height)));
  // NS_MAXSIZE means unconstrained.
  constexpr int kUnconstrained = 16777215;  // QWINDOWSIZE_MAX
  const int maxW = aConstraints.mMaxSize.width >= NS_MAXSIZE
                       ? kUnconstrained
                       : toDip(aConstraints.mMaxSize.width);
  const int maxH = aConstraints.mMaxSize.height >= NS_MAXSIZE
                       ? kUnconstrained
                       : toDip(aConstraints.mMaxSize.height);
  mQtWindow->setMaximumSize(QSize(maxW, maxH));
}

void nsWindow::SetTransparencyMode(TransparencyMode aMode) {
  mTransparencyMode = aMode;
  if (!mQtWindow) {
    return;
  }
  const bool transparent = aMode != TransparencyMode::Opaque;
  mQtWindow->SetTransparentBackground(transparent);
  if (transparent) {
    // Prefer per-pixel alpha over whole-window opacity so the panel can be
    // fully opaque while corners outside the rounded rect stay clear.
    QSurfaceFormat fmt = mQtWindow->format();
    if (fmt.alphaBufferSize() < 8) {
      fmt.setAlphaBufferSize(8);
      mQtWindow->setFormat(fmt);
    }
    mQtWindow->setOpacity(1.0);
  } else {
    mQtWindow->setOpacity(1.0);
  }
}

TransparencyMode nsWindow::GetTransparencyMode() { return mTransparencyMode; }

nsresult nsWindow::GetAttention(int32_t aCycleCount) {
  if (!mQtWindow) {
    return NS_ERROR_FAILURE;
  }
  // Flash taskbar / urgency hint. 0 = default platform duration.
  mQtWindow->alert(aCycleCount > 0 ? aCycleCount : 0);
  return NS_OK;
}

void nsWindow::ConstrainPosition(DesktopIntPoint& aPoint) {
  // Wayland places toplevels itself; constraining client coords is wrong.
  if (!mQtWindow || IsWaylandPlatform() || !IsTopLevelWidget()) {
    return;
  }

  const double dpiScale = GetDesktopToDeviceScale().scale;
  auto bounds = GetScreenBounds();
  int32_t logWidth =
      std::max(int32_t(std::lround(bounds.width / dpiScale)), 1);
  int32_t logHeight =
      std::max(int32_t(std::lround(bounds.height / dpiScale)), 1);

  nsCOMPtr<nsIScreenManager> screenmgr =
      do_GetService("@mozilla.org/gfx/screenmanager;1");
  if (!screenmgr) {
    return;
  }
  nsCOMPtr<nsIScreen> screen;
  screenmgr->ScreenForRect(aPoint.x, aPoint.y, logWidth, logHeight,
                           getter_AddRefs(screen));
  if (!screen) {
    return;
  }

  DesktopIntRect screenRect = mSizeMode == nsSizeMode_Fullscreen
                                  ? screen->GetRectDisplayPix()
                                  : screen->GetAvailRectDisplayPix();
  aPoint = ConstrainPositionToBounds(aPoint, {logWidth, logHeight}, screenRect);
}

void nsWindow::SetInputRegion(const InputRegion& aInputRegion) {
  mInputRegion = aInputRegion;
  ApplyInputRegionMask();
}

void nsWindow::ApplyInputRegionMask() {
  if (!mQtWindow) {
    return;
  }

  if (mInputRegion.mFullyTransparent) {
    mQtWindow->setFlag(Qt::WindowTransparentForInput, true);
    mQtWindow->setMask(QRegion());
    return;
  }

  mQtWindow->setFlag(Qt::WindowTransparentForInput, false);

  if (mInputRegion.mMargin == 0) {
    mQtWindow->setMask(QRegion());
#ifdef MOZ_X11
    // Clear X11 input shape if we previously set one.
    if (!IsWaylandPlatform() && mQtWindow->handle()) {
      if (auto* x11App =
              qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
        Display* dpy = x11App->display();
        Window xid = static_cast<Window>(mQtWindow->winId());
        if (dpy && xid) {
          XShapeCombineMask(dpy, xid, ShapeInput, 0, 0, None, ShapeSet);
        }
      }
    }
#endif
    return;
  }

  // Margin is in layout-device pixels; QWindow geometry is DIPs.
  const double dpr = DevicePixelRatio();
  const int margin = std::max(
      0, int(std::lround(double(mInputRegion.mMargin) / std::max(dpr, 1.0))));
  QRect r(0, 0, mQtWindow->width(), mQtWindow->height());
  r.adjust(margin, margin, -margin, -margin);
  if (r.width() <= 0 || r.height() <= 0) {
    mQtWindow->setFlag(Qt::WindowTransparentForInput, true);
    return;
  }

#ifdef MOZ_X11
  // Prefer input-only shape so transparent panel shadows stay painted.
  if (!IsWaylandPlatform() && mQtWindow->handle()) {
    if (auto* x11App =
            qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
      Display* dpy = x11App->display();
      Window xid = static_cast<Window>(mQtWindow->winId());
      if (dpy && xid) {
        XRectangle rect;
        rect.x = short(r.x());
        rect.y = short(r.y());
        rect.width = unsigned short(r.width());
        rect.height = unsigned short(r.height());
        XShapeCombineRectangles(dpy, xid, ShapeInput, 0, 0, &rect, 1, ShapeSet,
                                YXBanded);
        return;
      }
    }
  }
#endif
  // Wayland / fallback: mask affects both paint and input.
  mQtWindow->setMask(QRegion(r));
}

void nsWindow::CaptureRollupEvents(bool aDoCapture) {
  if (mIsDestroyed || !mQtWindow) {
    return;
  }
  // Match GTK: only grab the pointer for rare X11 WMs that move focus with
  // the pointer (without grab, entering a popup can roll it up).
  static const bool sNeedsGrab = [] {
    if (IsWaylandPlatform()) {
      return false;
    }
    const char* desktop = PR_GetEnv("XDG_CURRENT_DESKTOP");
    if (!desktop) {
      desktop = PR_GetEnv("DESKTOP_SESSION");
    }
    if (!desktop) {
      return false;
    }
    nsAutoCString d(desktop);
    d.StripWhitespace();
    ToLowerCase(d);
    return d.EqualsLiteral("twm") || d.EqualsLiteral("sawfish") ||
           StringBeginsWith(d, "fvwm"_ns);
  }();

  if (!sNeedsGrab) {
    mCapturingRollupEvents = aDoCapture;
    return;
  }

#ifdef MOZ_X11
  if (auto* x11App =
          qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
    Display* dpy = x11App->display();
    Window xid = static_cast<Window>(mQtWindow->winId());
    if (dpy && xid) {
      if (aDoCapture) {
        XGrabPointer(dpy, xid, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
      } else if (mCapturingRollupEvents) {
        XUngrabPointer(dpy, CurrentTime);
      }
      XFlush(dpy);
    }
  }
#endif
  mCapturingRollupEvents = aDoCapture;
}

void nsWindow::GetWorkspaceID(nsAString& aWorkspaceID) {
  aWorkspaceID.Truncate();
#ifdef MOZ_X11
  if (!mQtWindow || IsWaylandPlatform()) {
    return;
  }
  auto* x11App =
      qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
  if (!x11App) {
    return;
  }
  Display* dpy = x11App->display();
  Window xid = static_cast<Window>(mQtWindow->winId());
  if (!dpy || !xid) {
    return;
  }
  Atom netWmDesktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
  Atom actualType = None;
  int actualFormat = 0;
  unsigned long nitems = 0, bytesAfter = 0;
  unsigned char* prop = nullptr;
  if (XGetWindowProperty(dpy, xid, netWmDesktop, 0, 1, False, XA_CARDINAL,
                         &actualType, &actualFormat, &nitems, &bytesAfter,
                         &prop) == Success &&
      prop && nitems == 1 && actualFormat == 32) {
    aWorkspaceID.AppendInt(int32_t(*reinterpret_cast<long*>(prop)));
  }
  if (prop) {
    XFree(prop);
  }
#else
  (void)aWorkspaceID;
#endif
}

void nsWindow::MoveToWorkspace(const nsAString& aWorkspaceID) {
#ifdef MOZ_X11
  if (!mQtWindow || IsWaylandPlatform()) {
    return;
  }
  nsresult rv;
  const int32_t workspace = aWorkspaceID.ToInteger(&rv);
  if (NS_FAILED(rv)) {
    return;
  }
  auto* x11App =
      qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
  if (!x11App) {
    return;
  }
  Display* dpy = x11App->display();
  Window xid = static_cast<Window>(mQtWindow->winId());
  if (!dpy || !xid) {
    return;
  }
  Atom netWmDesktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
  XEvent xev;
  memset(&xev, 0, sizeof(xev));
  xev.type = ClientMessage;
  xev.xclient.window = xid;
  xev.xclient.message_type = netWmDesktop;
  xev.xclient.format = 32;
  xev.xclient.data.l[0] = workspace;
  xev.xclient.data.l[1] = 1;  // source indication: application
  Window root = DefaultRootWindow(dpy);
  XSendEvent(dpy, root, False,
             SubstructureNotifyMask | SubstructureRedirectMask, &xev);
  XFlush(dpy);
#else
  (void)aWorkspaceID;
#endif
}

void nsWindow::SetWindowClass(const nsAString& aXulWinType,
                              const nsAString& aXulWinClass,
                              const nsAString& aXulWinName) {
  if (!mQtWindow) {
    return;
  }

  // Qt exposes WM_CLASS via the QWindow title/file path on some platforms;
  // on X11 set WM_CLASS directly for session restore / window matching.
  nsAutoCString resName;
  nsAutoCString resClass;

  if (!aXulWinClass.IsEmpty()) {
    CopyUTF16toUTF8(aXulWinClass, resClass);
  }
  if (!aXulWinName.IsEmpty()) {
    CopyUTF16toUTF8(aXulWinName, resName);
  } else if (!aXulWinType.IsEmpty()) {
    CopyUTF16toUTF8(aXulWinType, resName);
    // Strip role after first ':' (same parsing as GTK).
    int32_t colon = resName.FindChar(':');
    if (colon >= 0) {
      resName.Truncate(colon);
    }
  }

  if (resName.IsEmpty() && resClass.IsEmpty()) {
    return;
  }
  if (resClass.IsEmpty()) {
    resClass.Assign(resName);
  }
  if (resName.IsEmpty()) {
    resName.Assign(resClass);
  }

  // Sanitize like GTK: [A-Za-z0-9_-] only.
  auto sanitize = [](nsACString& s) {
    for (uint32_t i = 0; i < s.Length(); ++i) {
      char c = s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) {
        s.Replace(i, 1, '_');
      }
    }
    if (!s.IsEmpty() && s[0] >= 'a' && s[0] <= 'z') {
      s.Replace(0, 1, char(s[0] - 'a' + 'A'));
    }
  };
  sanitize(resName);
  sanitize(resClass);

  mQtWindow->setProperty("_q_xdg_app_id",
                         QString::fromUtf8(resClass.get(), resClass.Length()));

#ifdef MOZ_X11
  if (!IsWaylandPlatform()) {
    if (auto* x11App =
            qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
      Display* dpy = x11App->display();
      Window xid = static_cast<Window>(mQtWindow->winId());
      if (dpy && xid) {
        XClassHint hint;
        hint.res_name = const_cast<char*>(resName.get());
        hint.res_class = const_cast<char*>(resClass.get());
        XSetClassHint(dpy, xid, &hint);
        XFlush(dpy);
      }
    }
  }
#endif
}

void nsWindow::SuppressAnimation(bool aSuppress) {
  // No DWM-style transition control on Qt/Linux. Remember the preference so
  // session-restore chrome can query intent if needed later.
  mIsAnimationSuppressed = aSuppress;
}

int nsWindow::CheckResizerEdge(const LayoutDeviceIntPoint& aPoint) const {
  if (mSizeMode != nsSizeMode_Normal || mPiPType != PiPType::MediaPiP) {
    return 0;
  }

  const int resizerHeight = int(std::lround(15.0 * DevicePixelRatio()));
  const int resizerWidth = resizerHeight * 4;
  const int topDist = aPoint.y;
  const int leftDist = aPoint.x;
  const int rightDist = mBounds.width - aPoint.x;
  const int bottomDist = mBounds.height - aPoint.y;

  // On Wayland with locked aspect ratio, north/west free resize fights the
  // compositor (same limitation as GTK).
  const bool waylandLimited =
      mAspectRatio > 0.0 && IsWaylandPlatform();

  int edges = 0;
  if (topDist <= resizerHeight) {
    if (rightDist <= resizerWidth) {
      edges = int(Qt::TopEdge | Qt::RightEdge);
    } else if (leftDist <= resizerWidth) {
      edges = int(Qt::TopEdge | Qt::LeftEdge);
    } else if (!waylandLimited) {
      edges = int(Qt::TopEdge);
    }
  } else if (bottomDist <= resizerHeight) {
    if (rightDist <= resizerWidth) {
      edges = int(Qt::BottomEdge | Qt::RightEdge);
    } else if (leftDist <= resizerWidth) {
      edges = int(Qt::BottomEdge | Qt::LeftEdge);
    } else {
      edges = int(Qt::BottomEdge);
    }
  } else if (leftDist <= resizerHeight) {
    if (!waylandLimited) {
      edges = int(Qt::LeftEdge);
    }
  } else if (rightDist <= resizerHeight) {
    edges = int(Qt::RightEdge);
  }
  return edges;
}

void nsWindow::SetResizeCursorForEdges(int aEdges) {
  if (!mQtWindow || !aEdges) {
    return;
  }
  const Qt::Edges e = Qt::Edges(aEdges);
  Qt::CursorShape shape = Qt::ArrowCursor;
  const bool top = e & Qt::TopEdge;
  const bool bottom = e & Qt::BottomEdge;
  const bool left = e & Qt::LeftEdge;
  const bool right = e & Qt::RightEdge;
  if ((top && left) || (bottom && right)) {
    shape = Qt::SizeFDiagCursor;
  } else if ((top && right) || (bottom && left)) {
    shape = Qt::SizeBDiagCursor;
  } else if (top || bottom) {
    shape = Qt::SizeVerCursor;
  } else if (left || right) {
    shape = Qt::SizeHorCursor;
  }
  mQtWindow->setCursor(QCursor(shape));
}

void nsWindow::TryToShowNativeWindowMenu(const QPointF& aGlobalPos) {
  if (!mQtWindow) {
    return;
  }
#ifdef MOZ_X11
  // Mutter / GTK X11 clients use _GTK_SHOW_WINDOW_MENU.
  if (!IsWaylandPlatform()) {
    if (auto* x11App =
            qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
      Display* dpy = x11App->display();
      Window xid = static_cast<Window>(mQtWindow->winId());
      if (dpy && xid) {
        Atom showMenu = XInternAtom(dpy, "_GTK_SHOW_WINDOW_MENU", False);
        XEvent xev;
        memset(&xev, 0, sizeof(xev));
        xev.type = ClientMessage;
        xev.xclient.window = xid;
        xev.xclient.message_type = showMenu;
        xev.xclient.format = 32;
        xev.xclient.data.l[0] = 0;  // device id
        xev.xclient.data.l[1] = long(std::lround(aGlobalPos.x()));
        xev.xclient.data.l[2] = long(std::lround(aGlobalPos.y()));
        Window root = DefaultRootWindow(dpy);
        XSendEvent(dpy, root, False,
                   SubstructureNotifyMask | SubstructureRedirectMask, &xev);
        XFlush(dpy);
        return;
      }
    }
  }
#endif
  // Wayland: no public QWindow API for xdg_toplevel.show_window_menu.
  (void)aGlobalPos;
}

void nsWindow::Invalidate(const LayoutDeviceIntRect&) {
  if (mQtWindow) {
    mQtWindow->requestUpdate();
  }
}

void* nsWindow::GetNativeData(uint32_t aDataType) {
  if (!mQtWindow) {
    return nullptr;
  }
  switch (aDataType) {
    case NS_NATIVE_WINDOW:
    case NS_NATIVE_WIDGET:
      if (!mQtWindow->handle()) {
        mQtWindow->create();
      }
      return reinterpret_cast<void*>(mQtWindow->winId());
    case NS_NATIVE_EGL_WINDOW:
      return reinterpret_cast<void*>(EnsureEGLNativeWindow());
    default:
      return nullptr;
  }
}

LayoutDeviceIntPoint nsWindow::WidgetToScreenOffset() {
  if (!mQtWindow) {
    return {};
  }
  // mapToGlobal returns DIPs; convert to layout-device pixels.
  return QtDipToDevice(mQtWindow->mapToGlobal(QPointF(0, 0)));
}

#ifdef MOZ_ENABLE_DBUS
void nsWindow::SetDBusMenuBar(
    RefPtr<mozilla::widget::DBusMenuBar> aDbusMenuBar) {
  mDBusMenuBar = std::move(aDbusMenuBar);
}
#endif

bool nsWindow::IsIMEEnabled() const {
  return mInputContext.mIMEState.mEnabled != IMEEnabled::Disabled;
}

bool nsWindow::IsIMEComposing() const {
  // GetTextEventDispatcher is non-const; IME composition state is safe to
  // observe through a const window.
  auto* self = const_cast<nsWindow*>(this);
  if (RefPtr<TextEventDispatcher> dispatcher = self->GetTextEventDispatcher()) {
    return dispatcher->IsComposing();
  }
  return false;
}

void nsWindow::UpdateInputMethod() {
  if (QInputMethod* im = QGuiApplication::inputMethod()) {
    im->update(Qt::ImQueryAll);
  }
}

QPointF nsWindow::DeviceToQtDip(const LayoutDeviceIntPoint& aPoint) const {
  const double dpr = DevicePixelRatio();
  if (dpr <= 0.0) {
    return QPointF(aPoint.x, aPoint.y);
  }
  return QPointF(aPoint.x / dpr, aPoint.y / dpr);
}

void nsWindow::SetInputContext(const InputContext& aContext,
                               const InputContextAction& aAction) {
  const bool wasEnabled = IsIMEEnabled();
  mInputContext = aContext;
  if (!mQtWindow) {
    return;
  }

  Qt::InputMethodHints hints = Qt::ImhNone;
  if (!IsIMEEnabled()) {
    hints |= Qt::ImhHiddenText | Qt::ImhNoPredictiveText;
  }
  if (aContext.mHTMLInputType.EqualsLiteral("password")) {
    hints |= Qt::ImhHiddenText | Qt::ImhNoAutoUppercase |
             Qt::ImhNoPredictiveText | Qt::ImhSensitiveData;
  } else if (aContext.mHTMLInputType.EqualsLiteral("email")) {
    hints |= Qt::ImhEmailCharactersOnly;
  } else if (aContext.mHTMLInputType.EqualsLiteral("url")) {
    hints |= Qt::ImhUrlCharactersOnly;
  } else if (aContext.mHTMLInputType.EqualsLiteral("number") ||
             aContext.mHTMLInputType.EqualsLiteral("tel")) {
    hints |= Qt::ImhFormattedNumbersOnly;
  } else if (aContext.mHTMLInputType.EqualsLiteral("search")) {
    // Prefer no auto-uppercase for search boxes.
    hints |= Qt::ImhNoAutoUppercase;
  }
  if (aContext.mHTMLInputMode.EqualsLiteral("numeric")) {
    hints |= Qt::ImhDigitsOnly;
  } else if (aContext.mHTMLInputMode.EqualsLiteral("decimal")) {
    hints |= Qt::ImhFormattedNumbersOnly;
  } else if (aContext.mHTMLInputMode.EqualsLiteral("tel")) {
    hints |= Qt::ImhDialableCharactersOnly;
  } else if (aContext.mHTMLInputMode.EqualsLiteral("email")) {
    hints |= Qt::ImhEmailCharactersOnly;
  } else if (aContext.mHTMLInputMode.EqualsLiteral("url")) {
    hints |= Qt::ImhUrlCharactersOnly;
  }

  mQtWindow->setProperty("_q_inputMethodHints", int(hints));

  if (QInputMethod* im = QGuiApplication::inputMethod()) {
    im->update(Qt::ImQueryAll);
    // Show virtual keyboard / notify IM engine when entering an editable field.
    if (IsIMEEnabled() && !wasEnabled) {
      im->show();
    } else if (!IsIMEEnabled() && wasEnabled) {
      im->hide();
      im->reset();
    }
  }
  (void)aAction;
}

InputContext nsWindow::GetInputContext() { return mInputContext; }

TextEventDispatcherListener*
nsWindow::GetNativeTextEventDispatcherListener() {
  return QtTextEventDispatcherListener::GetInstance();
}

bool nsWindow::GetEditCommands(mozilla::NativeKeyBindingsType aType,
                               const mozilla::WidgetKeyboardEvent& aEvent,
                               nsTArray<mozilla::CommandInt>& aCommands) {
  if (NS_WARN_IF(!nsIWidget::GetEditCommands(aType, aEvent, aCommands))) {
    return false;
  }
  Maybe<WritingMode> writingMode;
  if (aEvent.NeedsToRemapNavigationKey()) {
    if (RefPtr<TextEventDispatcher> dispatcher = GetTextEventDispatcher()) {
      writingMode = dispatcher->MaybeQueryWritingModeAtSelection();
    }
  }
  NativeKeyBindings* keyBindings = NativeKeyBindings::GetInstance(aType);
  keyBindings->GetEditCommands(aEvent, writingMode, aCommands);
  return true;
}

WindowRenderer* nsWindow::GetWindowRenderer() {
  return nsIWidget::GetWindowRenderer();
}

void nsWindow::SetCompositorWidgetDelegate(CompositorWidgetDelegate* aDelegate) {
  if (aDelegate) {
    mCompositorWidgetDelegate = aDelegate->AsPlatformSpecificDelegate();
    // Ensure SWGL has a non-zero client size as soon as the delegate exists.
    if (mCompositorWidgetDelegate && mBounds.width > 0 && mBounds.height > 0) {
      mCompositorWidgetDelegate->NotifyClientSizeChanged(mBounds.Size());
    }
  } else {
    mCompositorWidgetDelegate = nullptr;
  }
}

void nsWindow::DestroyWaylandEglWindow() {
  if (mWaylandEglWindow) {
    wl_egl_window_destroy(static_cast<wl_egl_window*>(mWaylandEglWindow));
    mWaylandEglWindow = nullptr;
    mWaylandEglSize = LayoutDeviceIntSize();
  }
}

void nsWindow::ResizeWaylandEglWindow(const LayoutDeviceIntSize& aSize) {
  if (!mWaylandEglWindow || aSize.width <= 0 || aSize.height <= 0) {
    return;
  }
  if (aSize == mWaylandEglSize) {
    return;
  }
  mWaylandEglSize = aSize;
  wl_egl_window_resize(static_cast<wl_egl_window*>(mWaylandEglWindow),
                       aSize.width, aSize.height, 0, 0);
}

void nsWindow::AbandonHardwarePresent() {
  if (!mUseHardwarePresent) {
    return;
  }
  mUseHardwarePresent = false;
  DestroyWaylandEglWindow();
  if (mQtWindow) {
    mQtWindow->AbandonHardwarePresent();
  }
}

uintptr_t nsWindow::EnsureEGLNativeWindow() {
  if (!mQtWindow || !mUseHardwarePresent) {
    return 0;
  }
  if (!mQtWindow->handle()) {
    mQtWindow->create();
  }

  const bool isWayland = QGuiApplication::platformName().contains(
      QLatin1String("wayland"), Qt::CaseInsensitive);

  if (!isWayland) {
    // X11/xcb: X Window id is the EGL native window.
    return static_cast<uintptr_t>(mQtWindow->winId());
  }

  // Wayland: exclusive OpenGLSurface windows only — never mix with QBackingStore.
  if (mWaylandEglWindow) {
    return reinterpret_cast<uintptr_t>(mWaylandEglWindow);
  }

  QPlatformNativeInterface* native =
      QGuiApplication::platformNativeInterface();
  if (!native) {
    return 0;
  }
  auto* surface = static_cast<wl_surface*>(
      native->nativeResourceForWindow("surface", mQtWindow.get()));
  if (!surface) {
    return 0;
  }

  int w = std::max(1, mBounds.width);
  int h = std::max(1, mBounds.height);
  if (w <= 1 && h <= 1) {
    w = std::max(1, mQtWindow->width());
    h = std::max(1, mQtWindow->height());
    const double dpr = DevicePixelRatio();
    w = std::max(1, int(std::lround(w * dpr)));
    h = std::max(1, int(std::lround(h * dpr)));
  }

  mWaylandEglWindow = wl_egl_window_create(surface, w, h);
  mWaylandEglSize = LayoutDeviceIntSize(w, h);
  return reinterpret_cast<uintptr_t>(mWaylandEglWindow);
}

void nsWindow::GetCompositorWidgetInitData(
    mozilla::widget::CompositorWidgetInitData* aInitData) {
  nsCString display;
  bool isX11 = true;
  if (QGuiApplication::platformName().contains(QLatin1String("wayland"),
                                               Qt::CaseInsensitive)) {
    isX11 = false;
  }
  if (qgetenv("DISPLAY").size()) {
    display.Assign(qgetenv("DISPLAY").constData());
  }
  const uintptr_t handle = EnsureEGLNativeWindow();
  *aInitData = QtCompositorWidgetInitData(
      handle, display, isX11,
      LayoutDeviceIntSize(mBounds.width, mBounds.height));
}

float nsWindow::GetDPI() {
  QScreen* screen = mQtWindow ? mQtWindow->screen() : nullptr;
  if (!screen) {
    screen = QGuiApplication::primaryScreen();
  }
  return PhysicalDpiForQScreen(screen);
}

double nsWindow::GetDefaultScaleInternal() { return DevicePixelRatio(); }

void nsWindow::OnScalePossiblyChanged() {
  // Popups inherit the transient parent's scale (see DevicePixelRatio()); the
  // top-level owns scale notifications — same model as GTK OnScaleEvent.
  if (IsPopup() || mIsDestroyed) {
    return;
  }

  const double dpr = DevicePixelRatio();
  if (mLastDevicePixelRatio > 0.0 &&
      std::fabs(mLastDevicePixelRatio - dpr) < 1e-6) {
    return;
  }

  const double oldDpr = mLastDevicePixelRatio;
  mLastDevicePixelRatio = dpr;

  // ScreenManager often sampled QScreen::devicePixelRatio()==1 at startup
  // before Wayland buffer scale was known. Rebuild so content processes get
  // the real CSS/desktop scale (half-size blurry pages when stuck at 1.0).
  mozilla::widget::ScreenHelperQt::RequestRefreshScreens();

  // Nothing to reflow yet (Create before AppShell chrome).
  if (!GetPresShell()) {
    return;
  }

  // First sample still at 1.0: common pre-map state; wait for the real scale
  // instead of thrashing layout. First sample already >1, or any later change,
  // must push UIResolutionChanged so content is not stuck half-size/blurry.
  if (oldDpr <= 0.0 && dpr == 1.0) {
    return;
  }

  NotifyAPZOfDPIChange();
  if (PresShell* presShell = GetPresShell()) {
    presShell->BackingScaleFactorChanged();
  }

  // Recompute device-pixel bounds under the new scale (DIP size usually
  // unchanged when only buffer scale flips).
  if (mQtWindow) {
    OnResize(mQtWindow->width(), mQtWindow->height());
  }
}

void nsWindow::OnExpose() {
  // Catch late Wayland scale after map when no resize was delivered.
  OnScalePossiblyChanged();

  // Hardware present: WebRender composites on vsync via EGL SwapBuffers.
  if (mUseHardwarePresent) {
    return;
  }
  // With a compositor, content is pushed via PresentSoftwareBuffer. Skip
  // PaintWindow once we have a presented frame (avoids present→expose loops).
  // Still paint when no frame exists yet — otherwise short-lived tooltips /
  // popups over video stay empty white until a present that may never come.
  if (mCompositorWidgetDelegate && mQtWindow && mQtWindow->HasPresentedFrame()) {
    return;
  }
  if (mWidgetListener) {
    mWidgetListener->PaintWindow(this);
  }
}

void nsWindow::OnResize(int aWidth, int aHeight) {
  // Qt resize sizes are DIPs; store layout-device (physical) size for Gecko.
  // Prefer live QWindow metrics so we never desync from the platform surface
  // (session restore / maximize can otherwise leave a stale DPR-scaled size).
  //
  // Detect DPR changes here too (resize often accompanies Wayland scale
  // updates). Do not recurse via OnScalePossiblyChanged→OnResize: track
  // scale inline and notify Gecko after bounds are updated.
  const double dpr = DevicePixelRatio();
  const bool scaleChanged =
      mLastDevicePixelRatio > 0.0 &&
      std::fabs(mLastDevicePixelRatio - dpr) >= 1e-6;
  if (mLastDevicePixelRatio <= 0.0 || scaleChanged) {
    mLastDevicePixelRatio = dpr;
    if (!IsPopup()) {
      mozilla::widget::ScreenHelperQt::RequestRefreshScreens();
    }
  }

  int dipW = aWidth;
  int dipH = aHeight;
  if (mQtWindow) {
    dipW = mQtWindow->width();
    dipH = mQtWindow->height();
  }

  // Under fractional DPR, lround(lround(dev/dpr)*dpr) is not idempotent and
  // can oscillate by 1 device pixel. If Gecko's current bounds already map to
  // the DIPs Qt reports, keep those device pixels (matches GTK stability and
  // stops pages from "shaking" on reflow/compositor ticks). Always recompute
  // when scale changed — old device bounds are in a different space.
  auto dipsFromDev = [dpr](int32_t dev) {
    return std::max(1, int(std::lround(double(dev) / dpr)));
  };
  int w;
  int h;
  if (!scaleChanged && mBounds.width > 0 && mBounds.height > 0 &&
      dipsFromDev(mBounds.width) == std::max(1, dipW) &&
      dipsFromDev(mBounds.height) == std::max(1, dipH)) {
    w = mBounds.width;
    h = mBounds.height;
  } else {
    w = std::max(1, int(std::lround(double(dipW) * dpr)));
    h = std::max(1, int(std::lround(double(dipH) * dpr)));
  }

  const LayoutDeviceIntRect oldBounds = mBounds;

  if (IsPopup()) {
    // First xdg_popup map often configures a temporary smaller size. Trust the
    // last size Gecko requested so the menu is not truncated; reopening works
    // because Gecko then already has the correct pref size.
    if (mGeckoPopupSize.width > 0 && mGeckoPopupSize.height > 0) {
      if (w < mGeckoPopupSize.width || h < mGeckoPopupSize.height) {
        w = mGeckoPopupSize.width;
        h = mGeckoPopupSize.height;
        mBounds.SizeTo(w, h);
        ApplyQtGeometryFromBounds();
      } else {
        mBounds.SizeTo(w, h);
        mGeckoPopupSize = mBounds.Size();
      }
    } else {
      mBounds.SizeTo(w, h);
    }
    // Do not rewrite popup position from QWindow::position() — parent-local
    // on Wayland and would corrupt global mBounds used for placement.
  } else {
    mBounds.SizeTo(w, h);
    // Keep top-left in sync with the actual window (fullscreen repositions).
    if (mQtWindow) {
      const QPoint pos = mQtWindow->position();
      const int curDipX = int(std::lround(double(oldBounds.x) / dpr));
      const int curDipY = int(std::lround(double(oldBounds.y) / dpr));
      if (!scaleChanged && curDipX == pos.x() && curDipY == pos.y()) {
        mBounds.MoveTo(oldBounds.TopLeft());
      } else {
        mBounds.MoveTo(int(std::lround(pos.x() * dpr)),
                       int(std::lround(pos.y() * dpr)));
      }
    }
  }

  // Avoid re-entering layout/compositor when nothing actually changed — but
  // never skip when DPR changed (content must reflow at the new scale).
  if (!scaleChanged && mBounds.IsEqualEdges(oldBounds)) {
    return;
  }

  ResizeWaylandEglWindow(mBounds.Size());
  if (mCompositorWidgetDelegate) {
    mCompositorWidgetDelegate->NotifyClientSizeChanged(mBounds.Size());
  }
  // Input-region margin is relative to the current window size.
  if (mInputRegion.mMargin != 0 || mInputRegion.mFullyTransparent) {
    ApplyInputRegionMask();
  }

  if (scaleChanged && !IsPopup()) {
    NotifyAPZOfDPIChange();
    if (PresShell* presShell = GetPresShell()) {
      presShell->BackingScaleFactorChanged();
    }
  }

  DispatchResizeEvent();
}

void nsWindow::OnWindowStateChanged(int aOldState, int aNewState) {
  if (!mQtWindow) {
    return;
  }
  const auto newStates = Qt::WindowStates(aNewState);
  nsSizeMode newMode = nsSizeMode_Normal;
  if (newStates & Qt::WindowFullScreen) {
    newMode = nsSizeMode_Fullscreen;
  } else if (newStates & Qt::WindowMinimized) {
    newMode = nsSizeMode_Minimized;
  } else if (newStates & Qt::WindowMaximized) {
    newMode = nsSizeMode_Maximized;
  }

  if (newMode == mSizeMode) {
    return;
  }

  // Entering fullscreen from the WM side (rare) — remember prior mode.
  if (newMode == nsSizeMode_Fullscreen && mSizeMode != nsSizeMode_Minimized) {
    mLastSizeModeBeforeFullscreen = mSizeMode;
  }
  // Leaving fullscreen via WM (e.g. user keybinding): restore tracking.
  if (mSizeMode == nsSizeMode_Fullscreen &&
      newMode != nsSizeMode_Fullscreen &&
      newMode != nsSizeMode_Minimized) {
    // Prefer the mode the WM reports after unfullscreen.
    mLastSizeModeBeforeFullscreen = nsSizeMode_Normal;
  }

  mSizeMode = newMode;
  if (mWidgetListener) {
    mWidgetListener->SizeModeChanged(mSizeMode);
  }

  // Approximate occlusion: minimized windows are fully occluded for the
  // compositor pause path; restored windows are unknown until more signals.
  if (newMode == nsSizeMode_Minimized) {
    NotifyOcclusionState(OcclusionState::OCCLUDED);
  } else if ((aOldState & int(Qt::WindowMinimized)) &&
             !(aNewState & int(Qt::WindowMinimized))) {
    NotifyOcclusionState(OcclusionState::VISIBLE);
  }
}

void nsWindow::OnMove(int aX, int aY) {
  // Moving across screens can change devicePixelRatio on multi-monitor.
  OnScalePossiblyChanged();
  LayoutDeviceIntPoint pos;
  if (IsPopup() && IsWaylandPlatform() && mTransientParentWidget) {
    // Event coords are parent-local on Wayland popups; store Gecko global.
    const LayoutDeviceIntPoint parentOrigin =
        mTransientParentWidget->WidgetToScreenOffset();
    const LayoutDeviceIntPoint local = QtDipToDevice(QPointF(aX, aY));
    pos = LayoutDeviceIntPoint(parentOrigin.x + local.x,
                               parentOrigin.y + local.y);
  } else if (IsPopup() && IsWaylandPlatform() && mQtWindow) {
    pos = QtDipToDevice(mQtWindow->mapToGlobal(QPointF(0, 0)));
  } else {
    const double dpr = DevicePixelRatio();
    pos = LayoutDeviceIntPoint(int(std::lround(aX * dpr)),
                               int(std::lround(aY * dpr)));
  }
  mBounds.MoveTo(pos);
  if (mWidgetListener) {
    mWidgetListener->WindowMoved(this, pos,
                                 nsIWidgetListener::ByMoveToRect::No);
  }
}

void nsWindow::OnFocus(bool aFocus) {
  // Popups must never become the "active" chrome window. Permission
  // doorhangers (PopupNotifications) ignore button clicks when
  // Services.focus.activeWindow is not the browser or the chrome document
  // matches :-moz-window-inactive.
  if (IsPopup()) {
    if (aFocus && mTransientParentWidget) {
      // Keep parent marked active for Gecko; do NOT requestActivate (loops).
      mTransientParentWidget->DispatchActivateEvent(true);
    }
    return;
  }

  if (!aFocus) {
    // Focus moved to one of our popups — do not deactivate the browser.
    // Important: only skip Gecko deactivation. Never raise/requestActivate
    // from here: on Wayland focusWindow() is often null and reclaim loops
    // forever, freezing all input (TB after account-hub Finish).
    if (QWindow* focused = QGuiApplication::focusWindow()) {
      if (auto* bridge = qobject_cast<QtWindowBridge*>(focused)) {
        if (nsWindow* w = bridge->GetWidget()) {
          if (w->IsPopup() && w->mTransientParentWidget == this) {
            return;
          }
          // Child dialog owns focus; leave parent Gecko-active state alone
          // for a11y/chrome styling only when it is a popup (above). Dialogs
          // should deactivate the parent normally.
        }
      }
    } else if (HasAnyVisibleTransientChild()) {
      // No-focus popup visible (WindowDoesNotAcceptFocus): keep chrome active.
      for (QWindow* win : QGuiApplication::allWindows()) {
        auto* bridge = qobject_cast<QtWindowBridge*>(win);
        if (!bridge || !win->isVisible()) {
          continue;
        }
        if (nsWindow* w = bridge->GetWidget()) {
          if (w->IsPopup() && w->mTransientParentWidget == this) {
            return;
          }
        }
      }
    }
  }

  DispatchActivateEvent(aFocus);
#ifdef ACCESSIBILITY
  if (aFocus) {
    DispatchActivateEventAccessible();
  } else {
    DispatchDeactivateEventAccessible();
  }
#endif
  if (aFocus && IsIMEEnabled()) {
    UpdateInputMethod();
  }
}

void nsWindow::OnWindowActivation(bool aActive) {
  // Track activation for Gecko only. Do not raise/requestActivate here —
  // that races the compositor and freezes input (see OnFocus).
  if (IsPopup() || mIsAlert || !ShouldAcceptActivate()) {
    return;
  }
  if (aActive) {
    DispatchActivateEvent(true);
  }
  // On deactivate: let OnFocus(false) decide whether to DispatchDeactivate.
}


static Modifiers QtModifiers(Qt::KeyboardModifiers aMods) {
  Modifiers mods = MODIFIER_NONE;
  if (aMods & Qt::ShiftModifier) {
    mods |= MODIFIER_SHIFT;
  }
  if (aMods & Qt::ControlModifier) {
    mods |= MODIFIER_CONTROL;
  }
  if (aMods & Qt::AltModifier) {
    mods |= MODIFIER_ALT;
  }
  if (aMods & Qt::MetaModifier) {
    mods |= MODIFIER_META;
  }
  return mods;
}

// Map Qt key + text into DOM KeyboardEvent fields for Gecko.
static void InitKeyboardEventFromQt(WidgetKeyboardEvent& aEvent,
                                    QKeyEvent* aQtEvent) {
  aEvent.mModifiers = QtModifiers(aQtEvent->modifiers());
  aEvent.mIsRepeat = aQtEvent->isAutoRepeat();

  const int key = aQtEvent->key();
  const QString text = aQtEvent->text();

  auto setNamedKey = [&](KeyNameIndex aName, uint32_t aKeyCode) {
    aEvent.mKeyNameIndex = aName;
    aEvent.mKeyCode = aKeyCode;
  };

  switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
      setNamedKey(KEY_NAME_INDEX_Enter, NS_VK_RETURN);
      return;
    case Qt::Key_Backspace:
      setNamedKey(KEY_NAME_INDEX_Backspace, NS_VK_BACK);
      return;
    case Qt::Key_Tab:
      setNamedKey(KEY_NAME_INDEX_Tab, NS_VK_TAB);
      return;
    case Qt::Key_Escape:
      setNamedKey(KEY_NAME_INDEX_Escape, NS_VK_ESCAPE);
      return;
    case Qt::Key_Delete:
      setNamedKey(KEY_NAME_INDEX_Delete, NS_VK_DELETE);
      return;
    case Qt::Key_Insert:
      setNamedKey(KEY_NAME_INDEX_Insert, NS_VK_INSERT);
      return;
    case Qt::Key_Home:
      setNamedKey(KEY_NAME_INDEX_Home, NS_VK_HOME);
      return;
    case Qt::Key_End:
      setNamedKey(KEY_NAME_INDEX_End, NS_VK_END);
      return;
    case Qt::Key_PageUp:
      setNamedKey(KEY_NAME_INDEX_PageUp, NS_VK_PAGE_UP);
      return;
    case Qt::Key_PageDown:
      setNamedKey(KEY_NAME_INDEX_PageDown, NS_VK_PAGE_DOWN);
      return;
    case Qt::Key_Left:
      setNamedKey(KEY_NAME_INDEX_ArrowLeft, NS_VK_LEFT);
      return;
    case Qt::Key_Up:
      setNamedKey(KEY_NAME_INDEX_ArrowUp, NS_VK_UP);
      return;
    case Qt::Key_Right:
      setNamedKey(KEY_NAME_INDEX_ArrowRight, NS_VK_RIGHT);
      return;
    case Qt::Key_Down:
      setNamedKey(KEY_NAME_INDEX_ArrowDown, NS_VK_DOWN);
      return;
    case Qt::Key_F1:
      setNamedKey(KEY_NAME_INDEX_F1, NS_VK_F1);
      return;
    case Qt::Key_F2:
      setNamedKey(KEY_NAME_INDEX_F2, NS_VK_F2);
      return;
    case Qt::Key_F3:
      setNamedKey(KEY_NAME_INDEX_F3, NS_VK_F3);
      return;
    case Qt::Key_F4:
      setNamedKey(KEY_NAME_INDEX_F4, NS_VK_F4);
      return;
    case Qt::Key_F5:
      setNamedKey(KEY_NAME_INDEX_F5, NS_VK_F5);
      return;
    case Qt::Key_F6:
      setNamedKey(KEY_NAME_INDEX_F6, NS_VK_F6);
      return;
    case Qt::Key_F7:
      setNamedKey(KEY_NAME_INDEX_F7, NS_VK_F7);
      return;
    case Qt::Key_F8:
      setNamedKey(KEY_NAME_INDEX_F8, NS_VK_F8);
      return;
    case Qt::Key_F9:
      setNamedKey(KEY_NAME_INDEX_F9, NS_VK_F9);
      return;
    case Qt::Key_F10:
      setNamedKey(KEY_NAME_INDEX_F10, NS_VK_F10);
      return;
    case Qt::Key_F11:
      setNamedKey(KEY_NAME_INDEX_F11, NS_VK_F11);
      return;
    case Qt::Key_F12:
      setNamedKey(KEY_NAME_INDEX_F12, NS_VK_F12);
      return;
    case Qt::Key_Shift:
      setNamedKey(KEY_NAME_INDEX_Shift, NS_VK_SHIFT);
      aEvent.mLocation = eKeyLocationLeft;
      return;
    case Qt::Key_Control:
      setNamedKey(KEY_NAME_INDEX_Control, NS_VK_CONTROL);
      aEvent.mLocation = eKeyLocationLeft;
      return;
    case Qt::Key_Alt:
      setNamedKey(KEY_NAME_INDEX_Alt, NS_VK_ALT);
      aEvent.mLocation = eKeyLocationLeft;
      return;
    case Qt::Key_Meta:
      setNamedKey(KEY_NAME_INDEX_Meta, NS_VK_WIN);
      aEvent.mLocation = eKeyLocationLeft;
      return;
    case Qt::Key_CapsLock:
      setNamedKey(KEY_NAME_INDEX_CapsLock, NS_VK_CAPS_LOCK);
      return;
    case Qt::Key_NumLock:
      setNamedKey(KEY_NAME_INDEX_NumLock, NS_VK_NUM_LOCK);
      return;
    case Qt::Key_ScrollLock:
      setNamedKey(KEY_NAME_INDEX_ScrollLock, NS_VK_SCROLL_LOCK);
      return;
    default:
      break;
  }

  // Printable / text-producing keys.
  if (!text.isEmpty()) {
    aEvent.mKeyNameIndex = KEY_NAME_INDEX_USE_STRING;
    aEvent.mKeyValue.Assign(reinterpret_cast<const char16_t*>(text.utf16()),
                            text.size());
    // Prefer Latin letter/digit keyCodes from Qt::Key when available.
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
      aEvent.mKeyCode = uint32_t(key);  // matches NS_VK_A..NS_VK_Z
    } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
      aEvent.mKeyCode = uint32_t(key);  // matches NS_VK_0..NS_VK_9
    } else if (key == Qt::Key_Space) {
      aEvent.mKeyCode = NS_VK_SPACE;
    } else {
      const char16_t ch = text[0].unicode();
      aEvent.mKeyCode = WidgetUtils::ComputeKeyCodeFromChar(uint32_t(ch));
    }
    return;
  }

  // Empty text: still map A–Z / 0–9 for Ctrl/Alt chords.
  if (key >= Qt::Key_A && key <= Qt::Key_Z) {
    aEvent.mKeyNameIndex = KEY_NAME_INDEX_USE_STRING;
    char16_t ch = char16_t(key);
    if (!(aEvent.mModifiers & MODIFIER_SHIFT)) {
      ch = char16_t(key - Qt::Key_A + 'a');
    }
    aEvent.mKeyValue.Assign(&ch, 1);
    aEvent.mKeyCode = uint32_t(key);
    return;
  }
  if (key >= Qt::Key_0 && key <= Qt::Key_9) {
    aEvent.mKeyNameIndex = KEY_NAME_INDEX_USE_STRING;
    char16_t ch = char16_t(key);
    aEvent.mKeyValue.Assign(&ch, 1);
    aEvent.mKeyCode = uint32_t(key);
    return;
  }

  aEvent.mKeyNameIndex = KEY_NAME_INDEX_Unidentified;
  aEvent.mKeyCode = 0;
}

void nsWindow::OnKeyEvent(QKeyEvent* aEvent, bool aDown) {
  nsUserIdleServiceQt::NotifyUserActivity();

  // While an IME composition is active, printable keys are handled via
  // QInputMethodEvent (preedit/commit). Still deliver navigation / control
  // keys (Escape, arrows, Enter for commit, etc.).
  const bool composing = IsIMEComposing();
  const int key = aEvent->key();
  const bool isPrintable =
      !aEvent->text().isEmpty() &&
      !(aEvent->modifiers() &
        (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
  const bool isIMEControlKey =
      key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape ||
      key == Qt::Key_Backspace || key == Qt::Key_Delete ||
      key == Qt::Key_Tab || key == Qt::Key_Backtab ||
      (key >= Qt::Key_Left && key <= Qt::Key_Down) ||
      key == Qt::Key_Home || key == Qt::Key_End || key == Qt::Key_PageUp ||
      key == Qt::Key_PageDown;
  if (composing && isPrintable && !isIMEControlKey) {
    // Let the IM engine consume; it will synthesize InputMethod events.
    aEvent->accept();
    return;
  }

  RefPtr<TextEventDispatcher> dispatcher = GetTextEventDispatcher();
  nsresult rv = dispatcher->BeginNativeInputTransaction();
  if (NS_FAILED(rv)) {
    return;
  }

  WidgetKeyboardEvent keyEvent(true, aDown ? eKeyDown : eKeyUp, this);
  InitKeyboardEventFromQt(keyEvent, aEvent);

  nsEventStatus status = nsEventStatus_eIgnore;
  dispatcher->DispatchKeyboardEvent(keyEvent.mMessage, keyEvent, status);

  // Character input (URL bar, content) needs eKeyPress after eKeyDown.
  // Skip during composition so we don't double-insert committed text.
  if (aDown && !composing && status != nsEventStatus_eConsumeNoDefault) {
    WidgetKeyboardEvent keyPressEvent(true, eKeyPress, this);
    InitKeyboardEventFromQt(keyPressEvent, aEvent);
    dispatcher->MaybeDispatchKeypressEvents(keyPressEvent, status);
  }
}

static mozilla::MouseButton QtButton(Qt::MouseButton aButton) {
  switch (aButton) {
    case Qt::LeftButton:
      return MouseButton::ePrimary;
    case Qt::RightButton:
      return MouseButton::eSecondary;
    case Qt::MiddleButton:
      return MouseButton::eMiddle;
    default:
      return MouseButton::eNotPressed;
  }
}

void nsWindow::OnMouseEvent(QMouseEvent* aEvent, bool aDown) {
  nsUserIdleServiceQt::NotifyUserActivity();
  const LayoutDeviceIntPoint point = QtDipToDevice(aEvent->position());
  const MouseButton button = QtButton(aEvent->button());

  // Media PiP: edge drag starts a compositor-driven system resize.
  if (aDown && button == MouseButton::ePrimary && mQtWindow) {
    const int edges = CheckResizerEdge(point);
    if (edges) {
      mQtWindow->startSystemResize(Qt::Edges(edges));
      return;
    }
  }

  if (aDown) {
    // Multi-click counting (double / triple).
    static const int32_t kClickDist = 5;
    const TimeStamp now = TimeStamp::Now();
    const bool sameButton = int32_t(button) == mLastClickButton;
    const bool nearLast =
        abs(point.x - mLastClickPoint.x) <= kClickDist &&
        abs(point.y - mLastClickPoint.y) <= kClickDist;
    // ~500ms system double-click interval.
    const bool quickEnough =
        !mLastClickTime.IsNull() &&
        (now - mLastClickTime).ToMilliseconds() < 500.0;
    if (sameButton && nearLast && quickEnough) {
      mClickCount++;
    } else {
      mClickCount = 1;
    }
    mLastClickButton = int32_t(button);
    mLastClickPoint = point;
    mLastClickTime = now;
  }

  WidgetMouseEvent event(true, aDown ? eMouseDown : eMouseUp, this,
                         WidgetMouseEvent::eReal);
  event.mRefPoint = point;
  event.mButton = button;
  event.mModifiers = QtModifiers(aEvent->modifiers());
  event.mClickCount = uint32_t(std::max(1, mClickCount));
  DispatchInputEvent(&event);

  // Right-click (or Ctrl+click) context menu on press.
  if (aDown && (button == MouseButton::eSecondary ||
                (button == MouseButton::ePrimary &&
                 (aEvent->modifiers() & Qt::ControlModifier)))) {
    // Media PiP: offer the WM window menu (always-on-top / all workspaces).
    if (button == MouseButton::eSecondary &&
        mPiPType == PiPType::MediaPiP && mQtWindow) {
      TryToShowNativeWindowMenu(aEvent->globalPosition());
    }
    WidgetMouseEvent context(true, eContextMenu, this, WidgetMouseEvent::eReal);
    context.mRefPoint = point;
    context.mButton = button;
    context.mModifiers = QtModifiers(aEvent->modifiers());
    context.mClickCount = 1;
    if (button == MouseButton::ePrimary) {
      context.mContextMenuTrigger = WidgetMouseEvent::eControlClick;
    }
    DispatchInputEvent(&context);
  }
}

void nsWindow::OnMouseMove(QMouseEvent* aEvent) {
  nsUserIdleServiceQt::NotifyUserActivity();
  const LayoutDeviceIntPoint point = QtDipToDevice(aEvent->position());

  // PiP edge resize cursors (locked while over the edge so layout can't reset).
  if (mPiPType == PiPType::MediaPiP) {
    const int edges = CheckResizerEdge(point);
    if (edges) {
      SetResizeCursorForEdges(edges);
      mWidgetCursorLocked = true;
    } else if (mWidgetCursorLocked) {
      mWidgetCursorLocked = false;
      mUpdateCursor = true;
      SetCursor(mCursor);
    }
  }

  WidgetMouseEvent event(true, eMouseMove, this, WidgetMouseEvent::eReal);
  event.mRefPoint = point;
  event.mModifiers = QtModifiers(aEvent->modifiers());
  DispatchInputEvent(&event);
}

void nsWindow::OnWheelEvent(QWheelEvent* aEvent) {
  nsUserIdleServiceQt::NotifyUserActivity();
  WidgetWheelEvent event(true, eWheel, this);
  event.mRefPoint = QtDipToDevice(aEvent->position());
  event.mModifiers = QtModifiers(aEvent->modifiers());

  // Prefer pixel deltas when the platform provides them (high-res / touchpad
  // scrolling). Line-only deltas feel stepped compared to Chromium.
  const QPoint pixelDelta = aEvent->pixelDelta();
  if (!pixelDelta.isNull()) {
    const double dpr = DevicePixelRatio();
    event.mDeltaX = pixelDelta.x() * dpr;
    event.mDeltaY = -pixelDelta.y() * dpr;
    event.mDeltaMode = dom::WheelEvent_Binding::DOM_DELTA_PIXEL;
  } else {
    const QPoint angle = aEvent->angleDelta();
    // Qt reports 1/8 degree units; 120 == one "line" notch.
    event.mDeltaX = angle.x() / 120.0;
    event.mDeltaY = -angle.y() / 120.0;
    event.mDeltaMode = dom::WheelEvent_Binding::DOM_DELTA_LINE;
  }

  // Touchpad / kinetic phases help APZ decide when to fling.
  switch (aEvent->phase()) {
    case Qt::ScrollBegin:
      event.mIsMomentum = false;
      break;
    case Qt::ScrollUpdate:
      break;
    case Qt::ScrollEnd:
      event.mIsMomentum = false;
      break;
    case Qt::NoScrollPhase:
    default:
      break;
  }
  if (aEvent->source() == Qt::MouseEventSynthesizedBySystem ||
      aEvent->inverted()) {
    // Inertial scrolling continuation from the compositor.
    if (aEvent->phase() == Qt::NoScrollPhase && pixelDelta.isNull()) {
      event.mIsMomentum = true;
    }
  }

  DispatchInputEvent(&event);
}

bool nsWindow::OnClose() {
  if (mWidgetListener) {
    mWidgetListener->RequestWindowClose(this);
    return true;
  }
  return false;
}

static void AssignQString(const QString& aSrc, nsAString& aDst) {
  aDst.Assign(reinterpret_cast<const char16_t*>(aSrc.utf16()), aSrc.size());
}

bool nsWindow::QuerySelectedText(nsAString& aText, uint32_t* aOffset) {
  aText.Truncate();
  WidgetQueryContentEvent query(true, eQuerySelectedText, this);
  query.InitForQuerySelectedText(SelectionType::eNormal);
  DispatchEvent(&query);
  if (query.Failed() || !query.mReply) {
    return false;
  }
  if (query.mReply->mOffsetAndData) {
    aText = query.mReply->DataRef();
    if (aOffset) {
      *aOffset = uint32_t(query.mReply->StartOffset());
    }
    return true;
  }
  return false;
}

bool nsWindow::QuerySurroundingText(nsAString& aText) {
  aText.Truncate();
  WidgetQueryContentEvent query(true, eQueryTextContent, this);
  query.InitForQueryTextContent(0, UINT32_MAX);
  DispatchEvent(&query);
  if (query.Failed() || !query.mReply || !query.mReply->mOffsetAndData) {
    return false;
  }
  aText = query.mReply->DataRef();
  return true;
}

bool nsWindow::QueryCaretRect(LayoutDeviceIntRect* aRect) {
  if (!aRect) {
    return false;
  }
  WidgetQueryContentEvent query(true, eQueryCaretRect, this);
  // -1 / relative-to-insertion-point: caret at current selection.
  WidgetQueryContentEvent::Options options;
  options.mRelativeToInsertionPoint = true;
  query.InitForQueryCaretRect(0, options);
  DispatchEvent(&query);
  if (query.Failed() || !query.mReply) {
    return false;
  }
  *aRect = query.mReply->mRect;
  return true;
}

void nsWindow::NotifyIMEOfFocusChange(bool aFocus) {
  mIMEEditorFocused = aFocus;
  if (!aFocus) {
    mIMESelectedText.Truncate();
    mIMEHasSelectionRange = false;
    mIMECaretRect = LayoutDeviceIntRect();
  }
  if (QInputMethod* im = QGuiApplication::inputMethod()) {
    if (aFocus && IsIMEEnabled()) {
      im->update(Qt::ImQueryAll);
      im->show();
    } else if (!aFocus) {
      im->hide();
      im->reset();
    }
  }
}

void nsWindow::NotifyIMEOfSelectionChange(
    const mozilla::widget::IMENotification& aNotification) {
  const auto& data = aNotification.mSelectionChangeData;
  if (data.IsInitialized() && data.HasRange()) {
    mIMESelectionOffset = data.StartOffset();
    mIMESelectionLength = data.Length();
    mIMESelectedText = data.String();
    mIMEHasSelectionRange = true;
  } else {
    mIMEHasSelectionRange = false;
    mIMESelectedText.Truncate();
  }
  // Refresh caret rect for candidate window placement.
  (void)QueryCaretRect(&mIMECaretRect);
  UpdateInputMethod();
}

void nsWindow::NotifyIMEOfTextChange(
    const mozilla::widget::IMENotification&) {
  (void)QueryCaretRect(&mIMECaretRect);
  UpdateInputMethod();
}

void nsWindow::NotifyIMEOfPositionChange() {
  (void)QueryCaretRect(&mIMECaretRect);
  UpdateInputMethod();
}

void nsWindow::RequestIMECommitComposition() {
  if (QInputMethod* im = QGuiApplication::inputMethod()) {
    im->commit();
  }
}

void nsWindow::RequestIMECancelComposition() {
  // Cancel: reset drops preedit without committing.
  if (QInputMethod* im = QGuiApplication::inputMethod()) {
    im->reset();
  }
  RefPtr<TextEventDispatcher> dispatcher = GetTextEventDispatcher();
  if (!dispatcher || !dispatcher->IsComposing()) {
    return;
  }
  if (NS_FAILED(dispatcher->BeginNativeInputTransaction())) {
    return;
  }
  nsEventStatus status = nsEventStatus_eIgnore;
  nsAutoString empty;
  dispatcher->CommitComposition(status, &empty);
}

void nsWindow::OnInputMethodEvent(QInputMethodEvent* aEvent) {
  if (!aEvent || !IsIMEEnabled()) {
    return;
  }
  RefPtr<TextEventDispatcher> dispatcher = GetTextEventDispatcher();
  if (NS_FAILED(dispatcher->BeginNativeInputTransaction())) {
    return;
  }

  nsEventStatus status = nsEventStatus_eIgnore;
  const QString commit = aEvent->commitString();
  const QString preedit = aEvent->preeditString();

  // Commit string first (may arrive together with empty preedit).
  if (!commit.isEmpty()) {
    nsAutoString text;
    AssignQString(commit, text);
    if (!dispatcher->IsComposing()) {
      // Some engines commit without a prior preedit.
      dispatcher->StartComposition(status);
      status = nsEventStatus_eIgnore;
    }
    dispatcher->CommitComposition(status, &text);
    UpdateInputMethod();
    return;
  }

  if (!preedit.isEmpty()) {
    nsAutoString text;
    AssignQString(preedit, text);
    if (!dispatcher->IsComposing()) {
      dispatcher->StartComposition(status);
      status = nsEventStatus_eIgnore;
    }
    dispatcher->SetPendingCompositionString(text);

    // Map Qt attributes to clause ranges (converted / raw / selected).
    bool hasClause = false;
    int32_t caret = int32_t(text.Length());
    int32_t selStart = -1;
    int32_t selLength = 0;
    for (const QInputMethodEvent::Attribute& attr : aEvent->attributes()) {
      if (attr.type == QInputMethodEvent::TextFormat) {
        // Qt uses QTextCharFormat in value; treat any format span as a clause.
        const int start = std::clamp(attr.start, 0, int(text.Length()));
        const int len =
            std::clamp(attr.length, 0, int(text.Length()) - start);
        if (len > 0) {
          // Heuristic: start of preedit is often "converted", rest is raw.
          TextRangeType type = (start == 0 && hasClause)
                                   ? TextRangeType::eConvertedClause
                                   : TextRangeType::eRawClause;
          dispatcher->AppendClauseToPendingComposition(uint32_t(len), type);
          hasClause = true;
        }
      } else if (attr.type == QInputMethodEvent::Cursor) {
        caret = std::clamp(attr.start, 0, int(text.Length()));
      } else if (attr.type == QInputMethodEvent::Selection) {
        selStart = std::clamp(attr.start, 0, int(text.Length()));
        selLength =
            std::clamp(attr.length, 0, int(text.Length()) - selStart);
      }
    }
    if (!hasClause) {
      dispatcher->AppendClauseToPendingComposition(text.Length(),
                                                   TextRangeType::eRawClause);
    }
    if (selStart >= 0 && selLength > 0) {
      // Selected clause in preedit (common for Japanese/Chinese IME).
      dispatcher->SetCaretInPendingComposition(uint32_t(selStart),
                                               uint32_t(selLength));
    } else {
      dispatcher->SetCaretInPendingComposition(uint32_t(caret), 0);
    }
    dispatcher->FlushPendingComposition(status);
    return;
  }

  // Empty preedit + empty commit: end composition (cancelled / finished).
  if (dispatcher->IsComposing()) {
    nsAutoString empty;
    dispatcher->CommitComposition(status, &empty);
  }
}

void nsWindow::OnInputMethodQuery(QInputMethodQueryEvent* aEvent) {
  if (!aEvent) {
    return;
  }
  const bool imEnabled = IsIMEEnabled();
  const Qt::InputMethodQueries queries = aEvent->queries();

  if (queries & Qt::ImEnabled) {
    aEvent->setValue(Qt::ImEnabled, imEnabled);
  }
  if (queries & Qt::ImHints) {
    aEvent->setValue(Qt::ImHints,
                     mQtWindow ? mQtWindow->property("_q_inputMethodHints")
                               : QVariant(int(Qt::ImhNone)));
  }
  if (queries & Qt::ImFont) {
    aEvent->setValue(Qt::ImFont, QGuiApplication::font());
  }
  if (queries & Qt::ImPreferredLanguage) {
    aEvent->setValue(Qt::ImPreferredLanguage, QLocale::system().name());
  }
  if (queries & Qt::ImInputItemClipRectangle) {
    if (mQtWindow) {
      aEvent->setValue(Qt::ImInputItemClipRectangle,
                       QRect(0, 0, mQtWindow->width(), mQtWindow->height()));
    }
  }

  if (!imEnabled) {
    return;
  }

  // Caret / candidate window rectangle (DIPs, window-local).
  if (queries & Qt::ImCursorRectangle) {
    LayoutDeviceIntRect caret = mIMECaretRect;
    if (caret.IsEmpty()) {
      (void)QueryCaretRect(&caret);
      mIMECaretRect = caret;
    }
    QRect rect;
    if (!caret.IsEmpty()) {
      // Convert widget-relative device pixels → DIP, then to local coords.
      // Query caret rect is typically screen or widget device pixels relative
      // to the widget origin.
      const QPointF topLeft = DeviceToQtDip(caret.TopLeft());
      const QPointF bottomRight =
          DeviceToQtDip(LayoutDeviceIntPoint(caret.XMost(), caret.YMost()));
      rect = QRect(QPoint(int(std::lround(topLeft.x())),
                          int(std::lround(topLeft.y()))),
                   QPoint(int(std::lround(bottomRight.x())),
                          int(std::lround(bottomRight.y()))));
      if (rect.height() < 1) {
        rect.setHeight(std::max(1, int(QGuiApplication::font().pixelSize())));
      }
    } else if (mQtWindow) {
      rect = QRect(0, 0, std::max(1, mQtWindow->width() / 4),
                   std::max(12, mQtWindow->height() / 30));
    }
    aEvent->setValue(Qt::ImCursorRectangle, rect);
  }

  if (queries & Qt::ImCursorPosition || queries & Qt::ImAnchorPosition ||
      queries & Qt::ImAbsolutePosition || queries & Qt::ImCurrentSelection ||
      queries & Qt::ImSurroundingText || queries & Qt::ImTextBeforeCursor ||
      queries & Qt::ImTextAfterCursor) {
    nsAutoString surrounding;
    const bool haveSurrounding = QuerySurroundingText(surrounding);

    uint32_t selOffset = mIMESelectionOffset;
    uint32_t selLength = mIMESelectionLength;
    nsString selected(mIMESelectedText);
    if (!mIMEHasSelectionRange) {
      uint32_t off = 0;
      if (QuerySelectedText(selected, &off)) {
        selOffset = off;
        selLength = selected.Length();
      }
    }

    if (queries & Qt::ImCursorPosition) {
      aEvent->setValue(Qt::ImCursorPosition, int(selOffset + selLength));
    }
    if (queries & Qt::ImAnchorPosition) {
      aEvent->setValue(Qt::ImAnchorPosition, int(selOffset));
    }
    if (queries & Qt::ImAbsolutePosition) {
      aEvent->setValue(Qt::ImAbsolutePosition, int(selOffset + selLength));
    }
    if (queries & Qt::ImCurrentSelection) {
      aEvent->setValue(
          Qt::ImCurrentSelection,
          QString::fromUtf16(
              reinterpret_cast<const char16_t*>(selected.BeginReading()),
              selected.Length()));
    }
    if (haveSurrounding) {
      const QString all = QString::fromUtf16(
          reinterpret_cast<const char16_t*>(surrounding.BeginReading()),
          surrounding.Length());
      if (queries & Qt::ImSurroundingText) {
        aEvent->setValue(Qt::ImSurroundingText, all);
      }
      if (queries & Qt::ImTextBeforeCursor) {
        const int pos = int(std::min(selOffset, uint32_t(all.size())));
        aEvent->setValue(Qt::ImTextBeforeCursor, all.left(pos));
      }
      if (queries & Qt::ImTextAfterCursor) {
        const int pos =
            int(std::min(selOffset + selLength, uint32_t(all.size())));
        aEvent->setValue(Qt::ImTextAfterCursor, all.mid(pos));
      }
    }
  }
}

static bool MimeDataHasBrowserFlavors(const QMimeData* aMime) {
  return aMime && (aMime->hasText() || aMime->hasHtml() || aMime->hasUrls() ||
                   aMime->hasImage());
}

static uint32_t QtDropActionToGecko(Qt::DropActions aActions,
                                    Qt::DropAction aPreferred) {
  // Prefer the action Qt proposes, then fall back to allowed set.
  auto one = [](Qt::DropAction a) -> uint32_t {
    switch (a) {
      case Qt::CopyAction:
        return nsIDragService::DRAGDROP_ACTION_COPY;
      case Qt::MoveAction:
        return nsIDragService::DRAGDROP_ACTION_MOVE;
      case Qt::LinkAction:
        return nsIDragService::DRAGDROP_ACTION_LINK;
      default:
        return nsIDragService::DRAGDROP_ACTION_NONE;
    }
  };
  uint32_t preferred = one(aPreferred);
  if (preferred != nsIDragService::DRAGDROP_ACTION_NONE) {
    return preferred;
  }
  uint32_t actions = nsIDragService::DRAGDROP_ACTION_NONE;
  if (aActions & Qt::CopyAction) {
    actions |= nsIDragService::DRAGDROP_ACTION_COPY;
  }
  if (aActions & Qt::MoveAction) {
    actions |= nsIDragService::DRAGDROP_ACTION_MOVE;
  }
  if (aActions & Qt::LinkAction) {
    actions |= nsIDragService::DRAGDROP_ACTION_LINK;
  }
  return actions ? actions : nsIDragService::DRAGDROP_ACTION_COPY;
}

static Qt::DropAction GeckoDragActionToQt(uint32_t aAction) {
  if (aAction & nsIDragService::DRAGDROP_ACTION_COPY) {
    return Qt::CopyAction;
  }
  if (aAction & nsIDragService::DRAGDROP_ACTION_MOVE) {
    return Qt::MoveAction;
  }
  if (aAction & nsIDragService::DRAGDROP_ACTION_LINK) {
    return Qt::LinkAction;
  }
  return Qt::IgnoreAction;
}

static already_AddRefed<nsIDragService> GetDragService() {
  nsCOMPtr<nsIDragService> ds =
      do_GetService("@mozilla.org/widget/dragservice;1");
  return ds.forget();
}

// Dispatch a Gecko drag event and return the session's resulting drop action.
// aStartSession: call StartDragSession (for eDragEnter).
static Qt::DropAction DispatchGeckoDrag(
    nsWindow* aWindow, EventMessage aMessage, const QPointF& aPos,
    Qt::DropActions aPossible, Qt::DropAction aProposed, const QMimeData* aMime,
    bool aStartSession) {
  nsCOMPtr<nsIDragService> dragService = GetDragService();
  if (!dragService) {
    return Qt::IgnoreAction;
  }

  nsCOMPtr<nsIDragSession> dragSession;
  if (aStartSession) {
    dragSession = dragService->StartDragSession(aWindow);
    if (nsDragSession* qs = static_cast<nsDragSession*>(dragSession.get())) {
      qs->SetDropMimeData(aMime);
    }
  } else {
    dragService->GetCurrentSession(aWindow, getter_AddRefs(dragSession));
  }
  if (!dragSession) {
    return Qt::IgnoreAction;
  }

  if (aMessage == eDragOver) {
    dragSession->FireDragEventAtSource(eDrag, 0);
    dragSession->SetCanDrop(false);
  } else if (aMessage == eDrop) {
    bool canDrop = false;
    if (NS_FAILED(dragSession->GetCanDrop(&canDrop)) || !canDrop) {
      // Not accepted — synthesize leave and end if external source.
      WidgetDragEvent exitEvent(true, eDragExit, aWindow);
      exitEvent.mRefPoint = aWindow->QtDipToDevice(aPos);
      aWindow->DispatchInputEvent(&exitEvent);
      nsCOMPtr<nsINode> sourceNode;
      dragSession->GetSourceNode(getter_AddRefs(sourceNode));
      if (!sourceNode) {
        dragSession->EndDragSession(false, 0);
      }
      return Qt::IgnoreAction;
    }
  }

  dragSession->SetDragAction(QtDropActionToGecko(aPossible, aProposed));

  WidgetDragEvent geckoEvent(true, aMessage, aWindow);
  geckoEvent.mRefPoint = aWindow->QtDipToDevice(aPos);
  aWindow->DispatchInputEvent(&geckoEvent);

  if (aMessage == eDragEnter || aMessage == eDragOver) {
    uint32_t dragAction = nsIDragService::DRAGDROP_ACTION_NONE;
    dragSession->GetDragAction(&dragAction);
    auto* qs = static_cast<nsDragSession*>(dragSession.get());
    int32_t childAction = qs->TakeChildProcessDragAction();
    if (childAction != nsIDragService::DRAGDROP_ACTION_UNINITIALIZED) {
      dragAction = uint32_t(childAction);
    }
    bool canDrop = false;
    dragSession->GetCanDrop(&canDrop);
    if (!canDrop && aMessage == eDragOver) {
      return Qt::IgnoreAction;
    }
    return GeckoDragActionToQt(dragAction);
  }

  if (aMessage == eDragExit || aMessage == eDrop) {
    nsCOMPtr<nsINode> sourceNode;
    dragSession->GetSourceNode(getter_AddRefs(sourceNode));
    if (!sourceNode) {
      // External app drag: session is done when we leave or drop.
      dragSession->EndDragSession(aMessage == eDrop, 0);
    }
  }
  return aProposed == Qt::IgnoreAction ? Qt::CopyAction : aProposed;
}

void nsWindow::OnDragEnter(QDragEnterEvent* aEvent) {
  if (!aEvent || !MimeDataHasBrowserFlavors(aEvent->mimeData())) {
    return;
  }
  const Qt::DropAction act = DispatchGeckoDrag(
      this, eDragEnter, aEvent->position(), aEvent->possibleActions(),
      aEvent->proposedAction(), aEvent->mimeData(), /* aStartSession */ true);
  if (act != Qt::IgnoreAction) {
    aEvent->setDropAction(act);
    aEvent->accept();
  }
}

void nsWindow::OnDragMove(QDragMoveEvent* aEvent) {
  if (!aEvent || !MimeDataHasBrowserFlavors(aEvent->mimeData())) {
    return;
  }
  const Qt::DropAction act = DispatchGeckoDrag(
      this, eDragOver, aEvent->position(), aEvent->possibleActions(),
      aEvent->proposedAction(), aEvent->mimeData(), /* aStartSession */ false);
  if (act != Qt::IgnoreAction) {
    aEvent->setDropAction(act);
    aEvent->accept();
  } else {
    aEvent->ignore();
  }
}

void nsWindow::OnDragLeave() {
  (void)DispatchGeckoDrag(this, eDragExit, QPointF(0, 0), Qt::IgnoreAction,
                          Qt::IgnoreAction, nullptr,
                          /* aStartSession */ false);
}

void nsWindow::OnDrop(QDropEvent* aEvent) {
  if (!aEvent || !aEvent->mimeData()) {
    return;
  }
  const Qt::DropAction act = DispatchGeckoDrag(
      this, eDrop, aEvent->position(), aEvent->possibleActions(),
      aEvent->proposedAction(), aEvent->mimeData(), /* aStartSession */ false);
  if (act != Qt::IgnoreAction) {
    aEvent->setDropAction(act);
    aEvent->accept();
  } else {
    aEvent->ignore();
  }
}

void nsWindow::DispatchResizeEvent() {
  const LayoutDeviceIntSize size(mBounds.width, mBounds.height);
  if (mWidgetListener) {
    mWidgetListener->WindowResized(this, size);
  }
  if (mAttachedWidgetListener) {
    mAttachedWidgetListener->WindowResized(this, size);
  }
}

void nsWindow::DispatchActivateEvent(bool aActivate) {
  if (!mWidgetListener) {
    return;
  }
  if (aActivate) {
    mWidgetListener->WindowActivated();
  } else {
    mWidgetListener->WindowDeactivated();
  }
}

#ifdef ACCESSIBILITY
void nsWindow::CreateRootAccessible() {
  if (!mRootAccessible) {
    mRootAccessible = GetRootAccessible();
  }
}

void nsWindow::DispatchEventToRootAccessible(uint32_t aEventType) {
  if (!a11y::ShouldA11yBeEnabled()) {
    return;
  }
  nsAccessibilityService* accService = GetOrCreateAccService();
  if (!accService) {
    return;
  }
  CreateRootAccessible();
  if (mRootAccessible) {
    accService->FireAccessibleEvent(aEventType, mRootAccessible);
  }
}

void nsWindow::DispatchActivateEventAccessible() {
  DispatchEventToRootAccessible(nsIAccessibleEvent::EVENT_WINDOW_ACTIVATE);
}

void nsWindow::DispatchDeactivateEventAccessible() {
  DispatchEventToRootAccessible(nsIAccessibleEvent::EVENT_WINDOW_DEACTIVATE);
}
#endif  // ACCESSIBILITY

already_AddRefed<nsIWidget> nsIWidget::CreateTopLevelWindow() {
  nsCOMPtr<nsIWidget> window = new nsWindow();
  return window.forget();
}

already_AddRefed<nsIWidget> nsIWidget::CreateChildWindow() {
  nsCOMPtr<nsIWidget> window = new nsWindow();
  return window.forget();
}
