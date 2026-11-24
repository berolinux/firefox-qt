/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScreenHelperQt.h"

#include "mozilla/RefPtr.h"
#include "mozilla/widget/Screen.h"
#include "nsTArray.h"

#include <cmath>

#include <QGuiApplication>
#include <QScreen>


namespace mozilla::widget {

static constexpr float kMMPerInch = 25.4f;

float PhysicalDpiForQScreen(QScreen* aScreen) {
  if (!aScreen) {
    return 96.0f;
  }
  const qreal dpr =
      aScreen->devicePixelRatio() > 0.0 ? aScreen->devicePixelRatio() : 1.0;
  const float fallback = float(96.0 * dpr);

  const QSizeF mm = aScreen->physicalSize();
  const QRect g = aScreen->geometry();
  // Gecko layout-device height = DIP geometry * scale.
  const int deviceHeight = int(std::lround(double(g.height()) * dpr));
  if (mm.height() <= 0.0 || deviceHeight <= 0) {
    const float phys = float(aScreen->physicalDotsPerInch());
    if (phys >= fallback * 0.5f && phys <= fallback * 2.0f) {
      return phys;
    }
    return fallback;
  }

  auto dpiFromMmHeight = [deviceHeight](qreal mmHeight) -> float {
    return float(deviceHeight) / (float(mmHeight) / kMMPerInch);
  };

  // 1) Compositor reports true millimetres.
  const float dpiRaw = dpiFromMmHeight(mm.height());
  // 2) Some HiDPI Wayland setups (observed on scaled TVs) report physicalSize
  // ~dpr times too large in each axis — undo that.
  const float dpiScaledMm =
      dpr > 1.0 ? dpiFromMmHeight(mm.height() / dpr) : dpiRaw;

  auto nearFallback = [fallback](float dpi) {
    return dpi >= fallback * 0.5f && dpi <= fallback * 2.0f;
  };
  // Prefer the reading closer to 96*dpr among plausible values. Large
  // displays legitimately sit well below 96*dpr (e.g. ~100–160 on a 4K TV
  // at 2× UI scale); still reject extreme nonsense (~37–74 from unscaled
  // double-sized mm).
  auto score = [fallback](float dpi) {
    return std::abs(dpi - fallback);
  };

  float best = fallback;
  float bestScore = score(fallback);
  for (float candidate : {dpiRaw, dpiScaledMm}) {
    if (!nearFallback(candidate)) {
      continue;
    }
    const float s = score(candidate);
    if (s < bestScore) {
      best = candidate;
      bestScore = s;
    }
  }
  return best;
}

ScreenHelperQt* ScreenHelperQt::sInstance = nullptr;

ScreenHelperQt::ScreenHelperQt() {
  sInstance = this;
  if (auto* app = qGuiApp) {
    connect(app, &QGuiApplication::screenAdded, this,
            &ScreenHelperQt::OnScreenAdded);
    connect(app, &QGuiApplication::screenRemoved, this,
            &ScreenHelperQt::OnScreenRemoved);
    connect(app, &QGuiApplication::primaryScreenChanged, this,
            &ScreenHelperQt::RefreshScreens);
    for (QScreen* qs : app->screens()) {
      ConnectScreen(qs);
    }
  }
  RefreshScreens();
}

ScreenHelperQt::~ScreenHelperQt() {
  if (sInstance == this) {
    sInstance = nullptr;
  }
  if (auto* app = qGuiApp) {
    for (QScreen* qs : app->screens()) {
      DisconnectScreen(qs);
    }
    disconnect(app, nullptr, this, nullptr);
  }
}

/* static */
void ScreenHelperQt::RequestRefreshScreens() {
  if (sInstance) {
    sInstance->RefreshScreens();
  }
}

void ScreenHelperQt::ConnectScreen(QScreen* aScreen) {
  if (!aScreen) {
    return;
  }
  // geometry / DPI signals are the closest thing Qt offers to "scale may have
  // changed" for QScreen; window-level DevicePixelRatioChange is handled in
  // nsWindow. Disconnect first so re-connect on screenAdded is idempotent.
  DisconnectScreen(aScreen);
  connect(aScreen, &QScreen::geometryChanged, this,
          &ScreenHelperQt::RefreshScreens);
  connect(aScreen, &QScreen::availableGeometryChanged, this,
          &ScreenHelperQt::RefreshScreens);
  connect(aScreen, &QScreen::physicalDotsPerInchChanged, this,
          &ScreenHelperQt::RefreshScreens);
  connect(aScreen, &QScreen::logicalDotsPerInchChanged, this,
          &ScreenHelperQt::RefreshScreens);
}

void ScreenHelperQt::DisconnectScreen(QScreen* aScreen) {
  if (!aScreen) {
    return;
  }
  disconnect(aScreen, nullptr, this, nullptr);
}

void ScreenHelperQt::OnScreenAdded(QScreen* aScreen) {
  ConnectScreen(aScreen);
  RefreshScreens();
}

void ScreenHelperQt::OnScreenRemoved(QScreen* aScreen) {
  DisconnectScreen(aScreen);
  RefreshScreens();
}

void ScreenHelperQt::RefreshScreens() {
  AutoTArray<RefPtr<Screen>, 4> screens;
  const bool isWayland = QGuiApplication::platformName().contains(
      QLatin1String("wayland"), Qt::CaseInsensitive);
  const auto qscreens = QGuiApplication::screens();
  for (QScreen* qs : qscreens) {
    if (!qs) {
      continue;
    }

    // QScreen geometry is in device-independent pixels; Gecko screen rects
    // are in layout-device (physical) pixels.
    const qreal dpr = qs->devicePixelRatio() > 0.0 ? qs->devicePixelRatio() : 1.0;
    // On Wayland, use available geometry as the screen rect (GTK does the
    // same) and zero the origin so multi-monitor offsets do not push menus
    // and popups outside the window (see GTK bug 1795066).
    const QRect g = isWayland ? qs->availableGeometry() : qs->geometry();
    const QRect ag = qs->availableGeometry();
    auto toDevice = [dpr](const QRect& r) {
      return LayoutDeviceIntRect(
          int32_t(std::lround(r.x() * dpr)),
          int32_t(std::lround(r.y() * dpr)),
          int32_t(std::lround(r.width() * dpr)),
          int32_t(std::lround(r.height() * dpr)));
    };
    LayoutDeviceIntRect rect = toDevice(g);
    LayoutDeviceIntRect avail = toDevice(ag);
    if (isWayland) {
      rect.MoveTo(0, 0);
      avail.MoveTo(0, 0);
    }

    const float dpi = PhysicalDpiForQScreen(qs);

    const DesktopToLayoutDeviceScale contentsScale(dpr);
    // CSS pixels -> layout device pixels must use the same scale so text and
    // content are rasterized at full device resolution (not upscaled later).
    const CSSToLayoutDeviceScale cssScale(dpr);

    int refreshRate = 0;
    const int mHz = qs->refreshRate();
    if (mHz > 0) {
      refreshRate = mHz;
    }

    // Qt does not report HDR metadata. 80 nits matches the SDR default used
    // by the GTK and Windows screen helpers.
    screens.AppendElement(MakeRefPtr<Screen>(
        rect, avail, 24, 24, uint32_t(refreshRate), contentsScale, cssScale,
        dpi, Screen::IsPseudoDisplay::No, Screen::IsHDR::No, 80.0f, 80.0f));
  }
  if (screens.IsEmpty()) {
    LayoutDeviceIntRect rect(0, 0, 1920, 1080);
    screens.AppendElement(MakeRefPtr<Screen>(
        rect, rect, 24, 24, 0, DesktopToLayoutDeviceScale(1.0),
        CSSToLayoutDeviceScale(1.0), 96.0f, Screen::IsPseudoDisplay::No,
        Screen::IsHDR::No, 80.0f, 80.0f));
  }
  ScreenManager::Refresh(std::move(screens));
}

}  // namespace mozilla::widget
