/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_nsWindow_h
#define widget_qt_nsWindow_h

#include "nsIWidget.h"
#include "mozilla/widget/CompositorWidget.h"
#include "mozilla/widget/IMEData.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/RefPtr.h"
#include "Units.h"
#include "nsString.h"

namespace mozilla {
class QtWaylandVsyncSource;
class VsyncDispatcher;
}  // namespace mozilla

class QKeyEvent;
class QMouseEvent;
class QPoint;
class QPointF;
class QSize;
class QWheelEvent;
class QInputMethodEvent;
class QInputMethodQueryEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QtWindowBridge;

namespace mozilla::widget {
class DBusMenuBar;
class PlatformCompositorWidgetDelegate;
}  // namespace mozilla::widget

#ifdef ACCESSIBILITY
namespace mozilla::a11y {
class LocalAccessible;
}  // namespace mozilla::a11y
#endif

class nsWindow final : public nsIWidget {
 public:
  nsWindow();

  NS_INLINE_DECL_REFCOUNTING_INHERITED(nsWindow, nsIWidget)

  static void ReleaseGlobals();
  static nsWindow* FromWidget(nsIWidget* aWidget);

  using nsIWidget::Create;
  [[nodiscard]] nsresult Create(nsIWidget* aParent,
                                const LayoutDeviceIntRect& aRect,
                                const mozilla::widget::InitData&) override;
  void Destroy() override;
  void Show(bool aState) override;
  bool IsVisible() const override;
  void Move(const DesktopPoint&) override;
  void Resize(const DesktopSize&, bool aRepaint) override;
  void Resize(const DesktopRect&, bool aRepaint) override;
  void Enable(bool aState) override;
  bool IsEnabled() const override;
  void SetFocus(Raise, mozilla::dom::CallerType aCallerType) override;
  LayoutDeviceIntRect GetBounds() override;
  LayoutDeviceIntRect GetClientBounds() override;
  LayoutDeviceIntPoint GetClientOffset() override;
  void SetSizeMode(nsSizeMode aMode) override;
  nsSizeMode SizeMode() override;
  [[nodiscard]] nsresult MakeFullScreen(bool aFullScreen) override;
  nsresult SetTitle(const nsAString& aTitle) override;
  void SetIcon(const nsAString& aIconSpec) override;
  void SetCursor(const Cursor& aCursor) override;
  void HideWindowChrome(bool aShouldHide) override;
  void SetModal(bool aModal) override;
  void SetSizeConstraints(const SizeConstraints& aConstraints) override;
  void SetTransparencyMode(TransparencyMode aMode) override;
  TransparencyMode GetTransparencyMode() override;
  void LockAspectRatio(bool aShouldLock) override;
  [[nodiscard]] nsresult GetAttention(int32_t aCycleCount) override;
  void Invalidate(const LayoutDeviceIntRect& aRect) override;
  void* GetNativeData(uint32_t aDataType) override;
  LayoutDeviceIntPoint WidgetToScreenOffset() override;
  void ConstrainPosition(DesktopIntPoint& aPoint) override;
  void SetInputRegion(const InputRegion& aInputRegion) override;
  void CaptureRollupEvents(bool aDoCapture) override;
  void NotifyOcclusionState(mozilla::widget::OcclusionState aState) override;
  void GetWorkspaceID(nsAString& aWorkspaceID) override;
  void MoveToWorkspace(const nsAString& aWorkspaceID) override;
  void SetWindowClass(const nsAString& aXulWinType,
                      const nsAString& aXulWinClass,
                      const nsAString& aXulWinName) override;
  void SuppressAnimation(bool aSuppress) override;
  void SetInputContext(const InputContext& aContext,
                       const InputContextAction& aAction) override;
  InputContext GetInputContext() override;
  TextEventDispatcherListener* GetNativeTextEventDispatcherListener() override;
  MOZ_CAN_RUN_SCRIPT bool GetEditCommands(
      mozilla::NativeKeyBindingsType aType,
      const mozilla::WidgetKeyboardEvent& aEvent,
      nsTArray<mozilla::CommandInt>& aCommands) override;
  WindowRenderer* GetWindowRenderer() override;
  void SetCompositorWidgetDelegate(CompositorWidgetDelegate*) override;
  void GetCompositorWidgetInitData(
      mozilla::widget::CompositorWidgetInitData* aInitData) override;
  float GetDPI() override;
  double GetDefaultScaleInternal() override;
  bool WidgetTypeSupportsAcceleration() override { return true; }

  // True when this window may become the active chrome window (not PiP/alert).
  bool ShouldAcceptActivate() const;

  void OnExpose();
  void OnResize(int aWidth, int aHeight);
  void OnMove(int aX, int aY);
  // aOldState / aNewState are Qt::WindowStates bitfields.
  void OnWindowStateChanged(int aOldState, int aNewState);
  void OnFocus(bool aFocus);
  void OnKeyEvent(QKeyEvent* aEvent, bool aDown);
  void OnMouseEvent(QMouseEvent* aEvent, bool aDown);
  void OnMouseMove(QMouseEvent* aEvent);
  void OnWheelEvent(QWheelEvent* aEvent);
  bool OnClose();
  void OnInputMethodEvent(QInputMethodEvent* aEvent);
  void OnInputMethodQuery(QInputMethodQueryEvent* aEvent);
  void OnDragEnter(QDragEnterEvent* aEvent);
  void OnDragMove(QDragMoveEvent* aEvent);
  void OnDragLeave();
  void OnDrop(QDropEvent* aEvent);

  // Called from QtTextEventDispatcherListener (Gecko → platform IME).
  void NotifyIMEOfFocusChange(bool aFocus);
  void NotifyIMEOfSelectionChange(
      const mozilla::widget::IMENotification& aNotification);
  void NotifyIMEOfTextChange(
      const mozilla::widget::IMENotification& aNotification);
  void NotifyIMEOfPositionChange();
  void RequestIMECommitComposition();
  void RequestIMECancelComposition();

  QtWindowBridge* GetQtWindow() const { return mQtWindow.get(); }

#ifdef MOZ_ENABLE_DBUS
  void SetDBusMenuBar(RefPtr<mozilla::widget::DBusMenuBar> aDbusMenuBar);
#endif

#ifdef ACCESSIBILITY
  void CreateRootAccessible();
  void DispatchEventToRootAccessible(uint32_t aEventType);
  void DispatchActivateEventAccessible();
  void DispatchDeactivateEventAccessible();
#endif

  // Qt uses device-independent pixels; Gecko layout device pixels are
  // physical (DPR-scaled). Helpers convert between the two spaces.
  double DevicePixelRatio() const;
  LayoutDeviceIntPoint QtDipToDevice(const QPointF& aDip) const;
  QPointF DeviceToQtDip(const LayoutDeviceIntPoint& aPoint) const;
  static bool IsWaylandPlatform();
  bool IsPopup() const { return mWindowType == WindowType::Popup; }
  // Top-level opaque windows use exclusive EGL present (no QBackingStore).
  bool UsesHardwarePresent() const { return mUseHardwarePresent; }
  // Drop exclusive EGL and restore software present (after EGL failure).
  void AbandonHardwarePresent();

  // Must match DevicePixelRatio so desktop↔device conversions (AppWindow
  // restore, Create, Move/Resize) stay consistent with CSS scale.
  mozilla::DesktopToLayoutDeviceScale GetDesktopToDeviceScale() const override {
    return mozilla::DesktopToLayoutDeviceScale(DevicePixelRatio());
  }

  // Native handle for EGL window surfaces (XID or wl_egl_window*).
  uintptr_t EnsureEGLNativeWindow();
  void ResizeWaylandEglWindow(const LayoutDeviceIntSize& aSize);

  void CreateCompositorVsyncDispatcher() override;
  RefPtr<mozilla::VsyncDispatcher> GetVsyncDispatcher() override;

 private:
  void MaybeInitWaylandVsync();
  ~nsWindow() override;

  void DispatchResizeEvent();
  void DispatchActivateEvent(bool aActivate);
  void ApplyQtGeometryFromBounds();
  // Wayland xdg_popup: map Gecko global device coords → parent-local DIPs.
  void ConfigureWaylandPopupPlacement(const QSize& aSizeDip);
  void DestroyWaylandEglWindow();
  void ApplyWindowDecoration(mozilla::widget::BorderStyle aStyle);
  void ApplyInputRegionMask();
  void ApplyKioskModeIfNeeded();
  // Returns Qt::Edges bitfield for MediaPiP system resize, or 0 if not on edge.
  int CheckResizerEdge(const LayoutDeviceIntPoint& aPoint) const;
  void TryToShowNativeWindowMenu(const QPointF& aGlobalPos);
  void SetResizeCursorForEdges(int aEdges);

  // Query Gecko editor state for QInputMethodQuery answers.
  bool QuerySelectedText(nsAString& aText, uint32_t* aOffset = nullptr);
  bool QuerySurroundingText(nsAString& aText);
  bool QueryCaretRect(LayoutDeviceIntRect* aRect);
  void UpdateInputMethod();
  bool IsIMEEnabled() const;
  bool IsIMEComposing() const;

  mozilla::UniquePtr<QtWindowBridge> mQtWindow;
  // Non-owning; set for popups/dialogs with a widget parent (Wayland placement).
  nsWindow* mTransientParentWidget = nullptr;
  LayoutDeviceIntRect mBounds;
  // Last size Gecko requested via Resize (device pixels). Wayland xdg_popup
  // configure can under-report size on first map; we never shrink below this.
  LayoutDeviceIntSize mGeckoPopupSize;
  nsSizeMode mSizeMode = nsSizeMode_Normal;
  // Restored when leaving DOM/chrome fullscreen via MakeFullScreen(false).
  nsSizeMode mLastSizeModeBeforeFullscreen = nsSizeMode_Normal;
  bool mIsVisible = false;
  bool mEnabled = true;
  bool mIsDestroyed = false;
  bool mWindowChromeHidden = false;
  bool mIsModal = false;
  bool mAlwaysOnTop = false;
  bool mIsAlert = false;
  bool mIsDragPopup = false;
  bool mUseHardwarePresent = false;
  bool mResizable = false;
  bool mIsAnimationSuppressed = false;
  bool mCapturingRollupEvents = false;
  bool mWidgetCursorLocked = false;
  // 0 = unlocked. Maintained for chrome "lockaspectratio" (PiP player).
  double mAspectRatio = 0.0;
  double mAspectRatioSaved = 0.0;
  TransparencyMode mTransparencyMode = TransparencyMode::Opaque;
  InputRegion mInputRegion;
  InputContext mInputContext;
  mozilla::widget::PlatformCompositorWidgetDelegate* mCompositorWidgetDelegate =
      nullptr;

  // Multi-click tracking (double/triple click).
  int32_t mLastClickButton = -1;
  int32_t mClickCount = 0;
  LayoutDeviceIntPoint mLastClickPoint;
  mozilla::TimeStamp mLastClickTime;

#ifdef ACCESSIBILITY
  RefPtr<mozilla::a11y::LocalAccessible> mRootAccessible;
#endif

  // IME / candidate-window placement and query cache.
  LayoutDeviceIntRect mIMECaretRect;
  nsString mIMESelectedText;
  uint32_t mIMESelectionOffset = 0;
  uint32_t mIMESelectionLength = 0;
  bool mIMEHasSelectionRange = false;
  bool mIMEEditorFocused = false;

  // Wayland: wl_egl_window* wrapping the QWindow wl_surface for EGL.
  void* mWaylandEglWindow = nullptr;
  LayoutDeviceIntSize mWaylandEglSize;

  // Per-window compositor vsync (Wayland frame callbacks).
  RefPtr<mozilla::QtWaylandVsyncSource> mWaylandVsyncSource;
  RefPtr<mozilla::VsyncDispatcher> mWaylandVsyncDispatcher;

#ifdef MOZ_ENABLE_DBUS
  // Plasma/Unity global menu export (com.canonical.dbusmenu).
  RefPtr<mozilla::widget::DBusMenuBar> mDBusMenuBar;
#endif
};

#endif
