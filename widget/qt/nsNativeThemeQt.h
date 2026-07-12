/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsNativeThemeQt_h_
#define nsNativeThemeQt_h_

#include "Theme.h"

/**
 * Native theme painting via QStyle so form controls match Breeze / the
 * active Qt style on Plasma and other Qt desktops.
 *
 * Uses an off-screen QWidget host so styles that sample widget palette/font
 * paint correctly. Falls back to cross-platform Theme when QStyle produces
 * nothing useful or color-scheme is forced away from the system.
 */
class nsNativeThemeQt final : public mozilla::widget::Theme {
  using Theme = mozilla::widget::Theme;

 public:
  nsNativeThemeQt();

  void DrawWidgetBackground(gfxContext*, nsIFrame*, StyleAppearance,
                            const nsRect& aRect, const nsRect& aDirtyRect,
                            DrawOverflow) override;

  bool CreateWebRenderCommandsForWidget(
      mozilla::wr::DisplayListBuilder&, mozilla::wr::IpcResourceUpdateQueue&,
      const mozilla::layers::StackingContextHelper&,
      mozilla::layers::RenderRootStateManager*, nsIFrame*, StyleAppearance,
      const nsRect&) override;

  [[nodiscard]] LayoutDeviceIntMargin GetWidgetBorder(nsDeviceContext*,
                                                      nsIFrame*,
                                                      StyleAppearance) override;

  bool GetWidgetPadding(nsDeviceContext*, nsIFrame*, StyleAppearance,
                        LayoutDeviceIntMargin*) override;

  mozilla::LayoutDeviceIntSize GetMinimumWidgetSize(nsPresContext*, nsIFrame*,
                                                    StyleAppearance) override;

  bool ThemeSupportsWidget(nsPresContext*, nsIFrame*, StyleAppearance) override;

  bool ThemeDrawsFocusForWidget(nsIFrame*, StyleAppearance) override;

  Transparency GetWidgetTransparency(nsIFrame*, StyleAppearance) override;

 private:
  ~nsNativeThemeQt() override = default;

  enum class PaintMode { QtStyle, FallbackTheme };

  static bool IsScrollbarPart(StyleAppearance aAppearance);
  static bool CanPaintWithQtStyle(StyleAppearance aAppearance);
  PaintMode ChoosePaintMode(nsIFrame* aFrame, StyleAppearance aAppearance);

  bool PaintQtWidget(gfxContext* aContext, nsIFrame* aFrame,
                     StyleAppearance aAppearance, const nsRect& aRect);
};

#endif
