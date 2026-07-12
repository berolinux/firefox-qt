/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_PLATFORM_QT_H
#define GFX_PLATFORM_QT_H

#include "gfxPlatform.h"

class gfxPlatformQt final : public gfxPlatform {
  friend class gfxPlatform;

 public:
  gfxPlatformQt();
  virtual ~gfxPlatformQt();

  static gfxPlatformQt* GetPlatform() {
    return static_cast<gfxPlatformQt*>(gfxPlatform::GetPlatform());
  }

  already_AddRefed<gfxASurface> CreateOffscreenSurface(
      const IntSize& aSize, gfxImageFormat aFormat) override;

  bool CreatePlatformFontList() override;

  void ReadSystemFontList(mozilla::dom::SystemFontList* aList) override;

  void GetCommonFallbackFonts(uint32_t aCh, Script aRunScript,
                              FontPresentation aPresentation,
                              nsTArray<const char*>& aFontList) override;

  bool SupportsApzWheelInput() const override { return true; }

  already_AddRefed<mozilla::gfx::VsyncSource> CreateGlobalHardwareVsyncSource()
      override;

  bool IsWaylandDisplay() override;

  static bool CheckVariationFontSupport();

 protected:
  bool AccelerateLayersByDefault() override { return true; }

  void InitAcceleration() override;
  void InitDmabufConfig();
  void InitPlatformHardwareVideoConfig() override;
};

// Wrapper for third party code (WebRTC) where gfxVars can't be included.
namespace mozilla::gfx {
bool IsDMABufEnabled();
}

#endif
