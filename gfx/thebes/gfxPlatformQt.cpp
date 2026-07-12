/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gfxPlatformQt.h"

#include "gfx2DGlue.h"
#include "gfxConfig.h"
#include "gfxFcPlatformFontList.h"
#include "gfxImageSurface.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/Components.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/widget/DMABufDevice.h"
#include "mozilla/widget/DMABufFormats.h"
#include "SoftwareVsyncSource.h"
#include "nsIGfxInfo.h"
#include "nsXULAppAPI.h"
#include "prenv.h"

#pragma GCC visibility push(default)
#include <QGuiApplication>
#include <QScreen>
#pragma GCC visibility pop

using namespace mozilla;
using namespace mozilla::gfx;
using namespace mozilla::widget;

static FT_Library gPlatformFTLibrary = nullptr;

// Highest monitor refresh rate in Hz (at least 60). Used for the software
// vsync timer so high-refresh displays are not capped at 60 Hz.
static double QtHighestScreenRefreshRateHz() {
  double best = gfxPlatform::GetSoftwareVsyncRate();
  if (!qGuiApp) {
    return best;
  }
  const QList<QScreen*> screens = QGuiApplication::screens();
  for (QScreen* screen : screens) {
    if (!screen) {
      continue;
    }
    // refreshRate() is 0 when unknown on some platforms.
    const qreal hz = screen->refreshRate();
    if (hz > best) {
      best = double(hz);
    }
  }
  // Clamp to a sane range (CI/tests assume not far below 60; upper bound
  // avoids pathological values from buggy drivers).
  if (best < 59.0) {
    best = 60.0;
  } else if (best > 360.0) {
    best = 360.0;
  }
  return best;
}

static TimeDuration QtVsyncPeriodFromScreens() {
  return TimeDuration::FromMilliseconds(1000.0 / QtHighestScreenRefreshRateHz());
}

gfxPlatformQt::gfxPlatformQt() {
  // Enable EGL-based WebRender compositing on Linux/Qt (X11 and Wayland QPA).
  // Content processes inherit this via gfxVars.
  if (XRE_IsParentProcess() && !IsHeadless()) {
    gfxVars::SetUseEGL(true);
    InitDmabufConfig();
    if (gfxConfig::IsEnabled(Feature::DMABUF)) {
      gfxVars::SetUseDMABuf(true);
    }
  }

  InitBackendPrefs(GetBackendPrefs());

  gPlatformFTLibrary = Factory::NewFTLibrary();
  MOZ_RELEASE_ASSERT(gPlatformFTLibrary);
  Factory::SetFTLibrary(gPlatformFTLibrary);

  // Temporary NVIDIA driver workaround (same as GTK).
  PR_SetEnv("__GL_ALLOW_FXAA_USAGE=0");
}

gfxPlatformQt::~gfxPlatformQt() {
  Factory::ReleaseFTLibrary(gPlatformFTLibrary);
  gPlatformFTLibrary = nullptr;
}

already_AddRefed<gfxASurface> gfxPlatformQt::CreateOffscreenSurface(
    const IntSize& aSize, gfxImageFormat aFormat) {
  if (!Factory::AllowedSurfaceSize(aSize)) {
    return nullptr;
  }
  RefPtr<gfxASurface> surface = new gfxImageSurface(aSize, aFormat);
  return surface.forget();
}

bool gfxPlatformQt::CreatePlatformFontList() {
  return gfxPlatformFontList::Initialize(new gfxFcPlatformFontList);
}

void gfxPlatformQt::ReadSystemFontList(mozilla::dom::SystemFontList* aList) {
  gfxFcPlatformFontList::PlatformFontList()->ReadSystemFontList(aList);
}

void gfxPlatformQt::GetCommonFallbackFonts(
    uint32_t, Script, FontPresentation, nsTArray<const char*>& aFontList) {
  aFontList.AppendElement("DejaVu Sans");
  aFontList.AppendElement("FreeSans");
  aFontList.AppendElement("Arial");
}

already_AddRefed<mozilla::gfx::VsyncSource>
gfxPlatformQt::CreateGlobalHardwareVsyncSource() {
  // Wayland: each toplevel gets a QtWaylandVsyncSource (frame callbacks).
  // The global source remains a rate-matched software timer for content
  // process refresh-driver plumbing and as a fallback before windows exist.
  // X11: same display-rate timer (XRandR via QScreen::refreshRate()).
  return MakeAndAddRef<SoftwareVsyncSource>(QtVsyncPeriodFromScreens());
}

bool gfxPlatformQt::IsWaylandDisplay() {
  if (IsHeadless()) {
    return false;
  }
  return QGuiApplication::platformName().contains(QLatin1String("wayland"),
                                                  Qt::CaseInsensitive);
}

bool gfxPlatformQt::CheckVariationFontSupport() { return true; }

void gfxPlatformQt::InitAcceleration() {
  gfxPlatform::InitAcceleration();

  if (XRE_IsContentProcess()) {
    ImportCachedContentDeviceData();
  }
}

void gfxPlatformQt::InitDmabufConfig() {
  FeatureState& feature = gfxConfig::GetFeature(Feature::DMABUF);
  feature.EnableByDefault();

  nsCString failureId;
  int32_t status;
  nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
  if (NS_FAILED(gfxInfo->GetFeatureStatus(nsIGfxInfo::FEATURE_DMABUF, failureId,
                                          &status))) {
    feature.Disable(FeatureStatus::BlockedNoGfxInfo, "gfxInfo is broken",
                    "FEATURE_FAILURE_NO_GFX_INFO"_ns);
  } else if (status != nsIGfxInfo::FEATURE_STATUS_OK) {
    feature.Disable(FeatureStatus::Blocklisted, "Blocklisted by gfxInfo",
                    failureId);
  }

  if (StaticPrefs::widget_dmabuf_force_enabled_AtStartup()) {
    feature.UserForceEnable("Force enabled by pref");
  } else if (!StaticPrefs::widget_dmabuf_enabled_AtStartup()) {
    feature.UserDisable("Force disable by pref",
                        "FEATURE_FAILURE_USER_FORCE_DISABLED"_ns);
  }

  if (!gfxVars::UseEGL()) {
    feature.ForceDisable(FeatureStatus::Unavailable, "Requires EGL",
                         "FEATURE_FAILURE_REQUIRES_EGL"_ns);
  }

  if (!gfxVars::WebglUseHardware()) {
    feature.Disable(FeatureStatus::Blocklisted,
                    "DMABuf disabled with software rendering", failureId);
  }

  nsAutoCString drmRenderDevice;
  gfxInfo->GetDrmRenderDevice(drmRenderDevice);
  gfxVars::SetDrmRenderDevice(drmRenderDevice);

  if (feature.IsEnabled()) {
    DMABufDeviceLock device;
    if (!device.GetDMABufDevice()->IsEnabled(failureId)) {
      feature.ForceDisable(FeatureStatus::Failed, "Failed to configure",
                           failureId);
    }
    (void)GetGlobalDMABufFormats();
  }
}

void gfxPlatformQt::InitPlatformHardwareVideoConfig() {
  FeatureState& featureDec =
      gfxConfig::GetFeature(Feature::HARDWARE_VIDEO_DECODING);
  if (!gfxVars::UseEGL()) {
    featureDec.ForceDisable(FeatureStatus::Unavailable, "Requires EGL",
                            "FEATURE_FAILURE_REQUIRES_EGL"_ns);
    gfxConfig::ForceDisable(Feature::HARDWARE_VIDEO_ENCODING,
                            FeatureStatus::Unavailable, "Requires EGL",
                            "FEATURE_FAILURE_REQUIRES_EGL"_ns);
  }

  if (!featureDec.IsEnabled()) {
    return;
  }

  FeatureState& featureZeroCopy =
      gfxConfig::GetFeature(Feature::HW_DECODED_VIDEO_ZERO_COPY);

  featureZeroCopy.EnableByDefault();
  uint32_t state =
      StaticPrefs::media_ffmpeg_vaapi_force_surface_zero_copy_AtStartup();
  if (state == 0) {
    featureZeroCopy.UserDisable("Force disable by pref",
                                "FEATURE_FAILURE_USER_FORCE_DISABLED"_ns);
  } else if (state == 1) {
    featureZeroCopy.UserEnable("Force enabled by pref");
  } else {
    nsCString failureId;
    int32_t status = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;
    nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
    if (NS_FAILED(gfxInfo->GetFeatureStatus(
            nsIGfxInfo::FEATURE_HW_DECODED_VIDEO_ZERO_COPY, failureId,
            &status))) {
      featureZeroCopy.Disable(FeatureStatus::BlockedNoGfxInfo,
                              "gfxInfo is broken",
                              "FEATURE_FAILURE_NO_GFX_INFO"_ns);
    } else if (status == nsIGfxInfo::FEATURE_BLOCKED_PLATFORM_TEST) {
      featureZeroCopy.ForceDisable(FeatureStatus::Unavailable,
                                   "Force disabled by gfxInfo", failureId);
    } else if (status != nsIGfxInfo::FEATURE_ALLOW_ALWAYS) {
      featureZeroCopy.Disable(FeatureStatus::Blocklisted,
                              "Blocklisted by gfxInfo", failureId);
    }
  }
  if (featureZeroCopy.IsEnabled()) {
    gfxVars::SetHwDecodedVideoZeroCopy(true);
  }
}

namespace mozilla::gfx {
bool IsDMABufEnabled() { return gfxVars::UseDMABuf(); }
}  // namespace mozilla::gfx
