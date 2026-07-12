/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_QtWindowBridge_h
#define widget_qt_QtWindowBridge_h

#pragma GCC visibility push(default)
#include <QWindow>
#include <QObject>
#include <QImage>
#include <QBackingStore>
#include <QRegion>
#pragma GCC visibility pop

#include "mozilla/UniquePtr.h"

class nsWindow;

// QWindow subclass that forwards native events to nsWindow.
class QtWindowBridge final : public QWindow {
  Q_OBJECT
 public:
  explicit QtWindowBridge(nsWindow* aWidget);
  ~QtWindowBridge() override;

  nsWindow* GetWidget() const { return mWidget; }
  void ClearWidget() { mWidget = nullptr; }

  // Exclusive EGL present: skip QBackingStore while true. Raster surface type
  // is kept so we can fall back if wl_egl_window/EGL fails.
  void ConfigureForHardwarePresent();
  void AbandonHardwarePresent();
  bool UsesHardwarePresent() const { return mHardwarePresent; }

  // Present a premultiplied ARGB32 image into the window (main thread only).
  // Takes ownership via move — callers should pass an owned QImage (or one
  // that keeps its buffer alive via QImage cleanup function).
  // aDamageDip is in window device-independent pixels; empty = full window.
  // No-op when UsesHardwarePresent() (EGL SwapBuffers owns presentation).
  void PresentImage(QImage aImage, const QRegion& aDamageDip = QRegion());

  // True after at least one PresentImage call (used to decide first expose).
  bool HasPresentedFrame() const {
    return mHardwarePresent || mBackingStoreValid || !mLastFrame.isNull();
  }

  // When true, clear the backing store to transparent each present so
  // rounded popup panels don't leave an opaque black frame.
  void SetTransparentBackground(bool aTransparent) {
    mTransparentBackground = aTransparent;
  }

 protected:
  bool event(QEvent* aEvent) override;
  void exposeEvent(QExposeEvent* aEvent) override;
  void resizeEvent(QResizeEvent* aEvent) override;
  void moveEvent(QMoveEvent* aEvent) override;
  void focusInEvent(QFocusEvent* aEvent) override;
  void focusOutEvent(QFocusEvent* aEvent) override;
  void keyPressEvent(QKeyEvent* aEvent) override;
  void keyReleaseEvent(QKeyEvent* aEvent) override;
  void mousePressEvent(QMouseEvent* aEvent) override;
  void mouseReleaseEvent(QMouseEvent* aEvent) override;
  void mouseMoveEvent(QMouseEvent* aEvent) override;
  void wheelEvent(QWheelEvent* aEvent) override;
  void closeEvent(QCloseEvent* aEvent) override;
  // QWindow has no dedicated IME/DnD virtuals; handle via event().

 private:
  void EnsureBackingStore();
  void FlushBackingStore();
  void PaintLastFrameToBackingStore(const QRegion& aDamageDip = QRegion());

  nsWindow* mWidget;
  mozilla::UniquePtr<QBackingStore> mBackingStore;
  QImage mLastFrame;
  bool mTransparentBackground = false;
  // True once the backing store has been painted; expose can flush without
  // retaining the full-frame QImage (frees SWGL buffers sooner).
  bool mBackingStoreValid = false;
  bool mHardwarePresent = false;
};

#endif
