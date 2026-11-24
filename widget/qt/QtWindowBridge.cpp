/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "QtWindowBridge.h"
#include "nsWindow.h"

#include <cmath>

#include <QEvent>
#include <QExposeEvent>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QCloseEvent>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QRegion>
#include <QGuiApplication>
#include <QInputMethod>
#include <QWindowStateChangeEvent>
#include <QSurfaceFormat>

QtWindowBridge::QtWindowBridge(nsWindow* aWidget) : mWidget(aWidget) {
  // Default: software WebRender path uses QBackingStore / raster surfaces.
  // ConfigureForHardwarePresent() switches top-levels to exclusive EGL.
  setSurfaceType(QSurface::RasterSurface);
  // Advertise IM support; QWindow delivers QEvent::InputMethod* when focused.
  setProperty("_q_inputMethodHints", int(Qt::ImhNone));
}

QtWindowBridge::~QtWindowBridge() { mWidget = nullptr; }

void QtWindowBridge::ConfigureForHardwarePresent() {
  // Keep RasterSurface so we can fall back to QBackingStore if EGL fails by
  // destroying wl_egl_window. Exclusive present simply never paints raster
  // while mHardwarePresent is true.
  mHardwarePresent = true;
  mBackingStore = nullptr;
  mBackingStoreValid = false;
}

void QtWindowBridge::AbandonHardwarePresent() {
  mHardwarePresent = false;
}

void QtWindowBridge::EnsureBackingStore() {
  if (mHardwarePresent) {
    return;
  }
  if (!mBackingStore) {
    mBackingStore = mozilla::MakeUnique<QBackingStore>(this);
  }
  if (mBackingStore->size() != size()) {
    mBackingStore->resize(size());
  }
}

void QtWindowBridge::PaintLastFrameToBackingStore(const QRegion& aDamageDip) {
  if (mHardwarePresent || mLastFrame.isNull() || !isExposed()) {
    return;
  }
  EnsureBackingStore();
  qreal dpr = mLastFrame.devicePixelRatio();
  if (dpr <= 0.0) {
    dpr = 1.0;
  }

  const QSizeF logical = QSizeF(mLastFrame.size()) / dpr;
  const bool scaleMismatch = qAbs(logical.width() - width()) > 1.0 ||
                             qAbs(logical.height() - height()) > 1.0;

  QRegion paintRegion;
  if (scaleMismatch || aDamageDip.isEmpty()) {
    paintRegion = QRegion(0, 0, width(), height());
  } else {
    paintRegion = aDamageDip.intersected(QRect(0, 0, width(), height()));
    if (paintRegion.isEmpty()) {
      return;
    }
  }

  mBackingStore->beginPaint(paintRegion);
  {
    QPainter painter(mBackingStore->paintDevice());
    // Source overwrites dest — correct for opaque chrome and avoids blend cost.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    if (mTransparentBackground) {
      for (const QRect& r : paintRegion) {
        painter.fillRect(r, Qt::transparent);
      }
    }
    if (scaleMismatch) {
      painter.drawImage(QRectF(0, 0, width(), height()), mLastFrame);
    } else {
      // 1:1 DIP: only redraw damaged rects (scrolling often dirty-bands).
      for (const QRect& r : paintRegion) {
        const QRect src(int(std::lround(r.x() * dpr)),
                        int(std::lround(r.y() * dpr)),
                        int(std::lround(r.width() * dpr)),
                        int(std::lround(r.height() * dpr)));
        painter.drawImage(r, mLastFrame, src);
      }
    }
  }
  mBackingStore->endPaint();
  mBackingStore->flush(paintRegion);
  mBackingStoreValid = true;
}

void QtWindowBridge::PresentImage(QImage aImage, const QRegion& aDamageDip) {
  if (mHardwarePresent) {
    // EGL SwapBuffers owns presentation for this window.
    return;
  }
  // Frame buffer is physical/device pixels (Gecko layout-device size).
  // Move, do not copy — callers already own a deep buffer (or wrap one).
  mLastFrame = std::move(aImage);
  qreal dpr = 1.0;
  if (mWidget) {
    dpr = qreal(mWidget->DevicePixelRatio());
  }
  if (dpr <= 0.0) {
    dpr = devicePixelRatio();
  }
  if (dpr <= 0.0) {
    dpr = 1.0;
  }
  mLastFrame.setDevicePixelRatio(dpr);
  PaintLastFrameToBackingStore(aDamageDip);
  // Drop pixel storage so SWGL ring buffers can be reused (backing store holds
  // the painted result for subsequent expose flushes).
  mLastFrame = QImage();
}

void QtWindowBridge::FlushBackingStore() {
  if (mHardwarePresent || !isExposed()) {
    return;
  }
  if (!mLastFrame.isNull()) {
    PaintLastFrameToBackingStore(QRegion());
    mLastFrame = QImage();
    return;
  }
  // Re-flush already-painted pixels without a full-frame QImage.
  if (mBackingStoreValid && mBackingStore) {
    EnsureBackingStore();
    mBackingStore->flush(QRegion(0, 0, width(), height()));
  }
}

bool QtWindowBridge::event(QEvent* aEvent) {
  if (!mWidget) {
    return QWindow::event(aEvent);
  }
  switch (aEvent->type()) {
    case QEvent::InputMethod:
      mWidget->OnInputMethodEvent(static_cast<QInputMethodEvent*>(aEvent));
      return true;
    case QEvent::InputMethodQuery: {
      auto* query = static_cast<QInputMethodQueryEvent*>(aEvent);
      mWidget->OnInputMethodQuery(query);
      return true;
    }
    case QEvent::DragEnter:
      mWidget->OnDragEnter(static_cast<QDragEnterEvent*>(aEvent));
      return true;
    case QEvent::DragMove:
      mWidget->OnDragMove(static_cast<QDragMoveEvent*>(aEvent));
      return true;
    case QEvent::DragLeave:
      mWidget->OnDragLeave();
      return true;
    case QEvent::Drop:
      mWidget->OnDrop(static_cast<QDropEvent*>(aEvent));
      return true;
    case QEvent::WindowStateChange: {
      auto* change = static_cast<QWindowStateChangeEvent*>(aEvent);
      const bool handled = QWindow::event(aEvent);
      mWidget->OnWindowStateChanged(int(change->oldState()),
                                    int(windowState()));
      return handled;
    }
    case QEvent::DevicePixelRatioChange:
      // Qt 6: fired when Wayland/X11 buffer scale for this window changes.
      // Without this, Gecko keeps the startup scale (often 1.0) and pages
      // render half-size and blurry on HiDPI.
      mWidget->OnScalePossiblyChanged();
      return QWindow::event(aEvent);
    case QEvent::WindowActivate:
      // Stacking/activation change (Wayland xdg_activation), not keyboard focus.
      mWidget->OnWindowActivation(true);
      break;
    case QEvent::WindowDeactivate:
      mWidget->OnWindowActivation(false);
      break;
    default:
      break;
  }
  return QWindow::event(aEvent);
}

void QtWindowBridge::exposeEvent(QExposeEvent* aEvent) {
  QWindow::exposeEvent(aEvent);
  if (isExposed() && !mHardwarePresent) {
    if (!mLastFrame.isNull()) {
      FlushBackingStore();
    } else if (mTransparentBackground) {
      // Avoid opaque white flash for alpha popups before the first Gecko frame.
      EnsureBackingStore();
      mBackingStore->beginPaint(QRegion(0, 0, width(), height()));
      {
        QPainter painter(mBackingStore->paintDevice());
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(QRectF(0, 0, width(), height()), Qt::transparent);
      }
      mBackingStore->endPaint();
      mBackingStore->flush(QRegion(0, 0, width(), height()));
    }
  }
  if (mWidget) {
    mWidget->OnExpose();
  }
}

void QtWindowBridge::resizeEvent(QResizeEvent* aEvent) {
  QWindow::resizeEvent(aEvent);
  if (!mHardwarePresent) {
    if (mBackingStore) {
      mBackingStore->resize(aEvent->size());
    }
    // Keep mBackingStoreValid / mLastFrame until the next present so Wayland
    // configure storms do not flash an empty store between frames.
  }
  if (mWidget) {
    mWidget->OnResize(aEvent->size().width(), aEvent->size().height());
  }
}

void QtWindowBridge::moveEvent(QMoveEvent* aEvent) {
  QWindow::moveEvent(aEvent);
  if (mWidget) {
    mWidget->OnMove(aEvent->pos().x(), aEvent->pos().y());
  }
}

void QtWindowBridge::focusInEvent(QFocusEvent* aEvent) {
  QWindow::focusInEvent(aEvent);
  if (mWidget) {
    mWidget->OnFocus(true);
  }
  if (QInputMethod* im = QGuiApplication::inputMethod()) {
    im->update(Qt::ImQueryAll);
  }
}

void QtWindowBridge::focusOutEvent(QFocusEvent* aEvent) {
  QWindow::focusOutEvent(aEvent);
  if (mWidget) {
    mWidget->OnFocus(false);
  }
}

void QtWindowBridge::keyPressEvent(QKeyEvent* aEvent) {
  if (mWidget) {
    mWidget->OnKeyEvent(aEvent, true);
  }
  QWindow::keyPressEvent(aEvent);
}

void QtWindowBridge::keyReleaseEvent(QKeyEvent* aEvent) {
  if (mWidget) {
    mWidget->OnKeyEvent(aEvent, false);
  }
  QWindow::keyReleaseEvent(aEvent);
}

void QtWindowBridge::mousePressEvent(QMouseEvent* aEvent) {
  // Activate only windows that should own chrome focus. Popups, Media PiP and
  // alerts must not steal activation (tab goes inactive, media pauses, etc.).
  // raise() matters on KDE/Wayland: requestActivate alone often does not
  // restack after a no-focus menu demoted the surface.
  if (mWidget && mWidget->ShouldAcceptActivate()) {
    raise();
    requestActivate();
  }
  if (mWidget) {
    mWidget->OnMouseEvent(aEvent, true);
  }
  QWindow::mousePressEvent(aEvent);
}

void QtWindowBridge::mouseReleaseEvent(QMouseEvent* aEvent) {
  if (mWidget) {
    mWidget->OnMouseEvent(aEvent, false);
  }
  QWindow::mouseReleaseEvent(aEvent);
}

void QtWindowBridge::mouseMoveEvent(QMouseEvent* aEvent) {
  if (mWidget) {
    mWidget->OnMouseMove(aEvent);
  }
  QWindow::mouseMoveEvent(aEvent);
}

void QtWindowBridge::wheelEvent(QWheelEvent* aEvent) {
  if (mWidget) {
    mWidget->OnWheelEvent(aEvent);
  }
  QWindow::wheelEvent(aEvent);
}

void QtWindowBridge::closeEvent(QCloseEvent* aEvent) {
  if (mWidget && mWidget->OnClose()) {
    aEvent->ignore();
    return;
  }
  QWindow::closeEvent(aEvent);
}
