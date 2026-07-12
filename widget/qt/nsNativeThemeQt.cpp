/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsNativeThemeQt.h"

#include "QtPrefUtils.h"
#include "gfxContext.h"
#include "gfxPlatform.h"
#include "mozilla/dom/HTMLInputElement.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/LookAndFeel.h"
#include "nsDeviceContext.h"
#include "nsIFrame.h"
#include "nsPresContext.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#pragma GCC visibility push(default)
#include <QAbstractSpinBox>
#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QStyleOptionFocusRect>
#include <QStyleOptionFrame>
#include <QStyleOptionProgressBar>
#include <QStyleOptionSlider>
#include <QStyleOptionSpinBox>
#include <QWidget>
#include <QRgb>
#pragma GCC visibility pop

using namespace mozilla;
using namespace mozilla::gfx;
using namespace mozilla::widget;

nsNativeThemeQt::nsNativeThemeQt() : Theme(ScrollbarStyle()) {}

// Off-screen QWidget so styles that sample the widget (Breeze, Oxygen, …)
// get a real palette/font/direction instead of painting empty pixels.
static QWidget* StylePaintHost() {
  static QWidget* sHost = nullptr;
  if (!QApplication::instance()) {
    return nullptr;
  }
  if (!sHost) {
    sHost = new QWidget(nullptr);
    sHost->setAttribute(Qt::WA_DontShowOnScreen, true);
    sHost->setAttribute(Qt::WA_QuitOnClose, false);
    sHost->setAttribute(Qt::WA_TranslucentBackground, true);
    sHost->setAutoFillBackground(false);
    sHost->resize(1, 1);
    sHost->ensurePolished();
  }
  sHost->setPalette(QApplication::palette());
  sHost->setFont(QApplication::font());
  sHost->setLayoutDirection(QApplication::layoutDirection());
  if (QStyle* appStyle = QApplication::style()) {
    sHost->setStyle(appStyle);
  }
  return sHost;
}

static bool ImageHasVisiblePixels(const QImage& aImage) {
  for (int y = 0; y < aImage.height(); ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(aImage.constScanLine(y));
    for (int x = 0; x < aImage.width(); ++x) {
      if (qAlpha(line[x]) > 8) {
        return true;
      }
    }
  }
  return false;
}

/* static */
bool nsNativeThemeQt::IsScrollbarPart(StyleAppearance aAppearance) {
  return nsNativeTheme::IsWidgetScrollbarPart(aAppearance);
}

/* static */
bool nsNativeThemeQt::CanPaintWithQtStyle(StyleAppearance aAppearance) {
  switch (aAppearance) {
    case StyleAppearance::Button:
    case StyleAppearance::Toolbarbutton:
    case StyleAppearance::Textfield:
    case StyleAppearance::Searchfield:
    case StyleAppearance::NumberInput:
    case StyleAppearance::PasswordInput:
    case StyleAppearance::Textarea:
    case StyleAppearance::Menulist:
    case StyleAppearance::MenulistButton:
    case StyleAppearance::Listbox:
    case StyleAppearance::MozMenulistArrowButton:
    case StyleAppearance::Menupopup:
    case StyleAppearance::ProgressBar:
    case StyleAppearance::Meter:
    case StyleAppearance::Range:
    case StyleAppearance::SpinnerUpbutton:
    case StyleAppearance::SpinnerDownbutton:
    case StyleAppearance::Checkbox:
    case StyleAppearance::Radio:
    case StyleAppearance::Tooltip:
    case StyleAppearance::FocusOutline:
    case StyleAppearance::ScrollbarVertical:
    case StyleAppearance::ScrollbarHorizontal:
    case StyleAppearance::ScrollbarthumbVertical:
    case StyleAppearance::ScrollbarthumbHorizontal:
    case StyleAppearance::ScrollbarbuttonUp:
    case StyleAppearance::ScrollbarbuttonDown:
    case StyleAppearance::ScrollbarbuttonLeft:
    case StyleAppearance::ScrollbarbuttonRight:
    case StyleAppearance::Scrollcorner:
      return true;
    default:
      return false;
  }
}

nsNativeThemeQt::PaintMode nsNativeThemeQt::ChoosePaintMode(
    nsIFrame* aFrame, StyleAppearance aAppearance) {
  if (!PrefNativeThemeControls()) {
    return PaintMode::FallbackTheme;
  }
  if (!CanPaintWithQtStyle(aAppearance)) {
    return PaintMode::FallbackTheme;
  }
  if (IsWidgetStyled(aFrame->PresContext(), aFrame, aAppearance)) {
    return PaintMode::FallbackTheme;
  }
  // Prefer QStyle when the frame's color-scheme matches the system. Forced
  // light content on a dark Plasma session falls back to Theme so contrast
  // stays correct.
  const ColorScheme frameScheme = LookAndFeel::ColorSchemeForFrame(aFrame);
  const ColorScheme systemScheme = LookAndFeel::SystemColorScheme();
  if (frameScheme != systemScheme) {
    if (!Theme::ThemeSupportsWidget(aFrame->PresContext(), aFrame,
                                    aAppearance)) {
      return PaintMode::QtStyle;
    }
    return PaintMode::FallbackTheme;
  }
  if (!QApplication::instance() || !QApplication::style()) {
    return PaintMode::FallbackTheme;
  }
  return PaintMode::QtStyle;
}

static QStyle::State ElementStateToQtState(const dom::ElementState& aState,
                                           StyleAppearance aAppearance) {
  QStyle::State state = QStyle::State_None;
  if (!aState.HasState(dom::ElementState::DISABLED)) {
    state |= QStyle::State_Enabled;
  }
  if (aState.HasState(dom::ElementState::HOVER)) {
    state |= QStyle::State_MouseOver;
  }
  if (aState.HasState(dom::ElementState::ACTIVE)) {
    state |= QStyle::State_Sunken;
  }
  if (aState.HasState(dom::ElementState::FOCUS) ||
      aState.HasState(dom::ElementState::FOCUSRING)) {
    state |= QStyle::State_HasFocus | QStyle::State_KeyboardFocusChange;
  }
  if (aState.HasState(dom::ElementState::CHECKED)) {
    state |= QStyle::State_On;
  } else if (aAppearance == StyleAppearance::Checkbox ||
             aAppearance == StyleAppearance::Radio) {
    state |= QStyle::State_Off;
  }
  if (aState.HasState(dom::ElementState::INDETERMINATE)) {
    state |= QStyle::State_NoChange;
  }
  return state;
}

static void DrawImageToContext(gfxContext* aContext, const QImage& aImage,
                               const gfxPoint& aDest, bool aSnapped) {
  if (aImage.isNull() || aImage.width() <= 0 || aImage.height() <= 0) {
    return;
  }

  QImage converted = aImage;
  if (converted.format() != QImage::Format_ARGB32_Premultiplied &&
      converted.format() != QImage::Format_ARGB32) {
    converted = converted.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  }

  const IntSize size(converted.width(), converted.height());
  RefPtr<DataSourceSurface> surface = Factory::CreateDataSourceSurface(
      size, SurfaceFormat::B8G8R8A8, /* aZero = */ false);
  if (!surface) {
    return;
  }
  DataSourceSurface::ScopedMap map(surface, DataSourceSurface::WRITE);
  if (!map.IsMapped()) {
    return;
  }

  const int srcStride = converted.bytesPerLine();
  const int dstStride = map.GetStride();
  const int rowBytes = size.width * 4;
  for (int y = 0; y < size.height; ++y) {
    memcpy(map.GetData() + y * dstStride, converted.constScanLine(y),
           std::min(rowBytes, srcStride));
  }

  DrawTarget* dt = aContext->GetDrawTarget();
  const Point dest = aSnapped ? Point(NS_lround(aDest.x), NS_lround(aDest.y))
                              : ToPoint(aDest);
  dt->DrawSurface(surface, Rect(dest, Size(size.width, size.height)),
                  Rect(Point(), Size(size.width, size.height)));
}

static void InitCommonOption(QStyleOption& aOpt, QWidget* aHost,
                             const QRect& aRect, QStyle::State aState) {
  if (aHost) {
    aOpt.initFrom(aHost);
  } else {
    aOpt.palette = QApplication::palette();
    aOpt.fontMetrics = QFontMetrics(QApplication::font());
    aOpt.direction = QApplication::layoutDirection();
  }
  aOpt.rect = aRect;
  aOpt.state = aState;
  aOpt.palette = QApplication::palette();
  aOpt.styleObject = aHost;
}

bool nsNativeThemeQt::PaintQtWidget(gfxContext* aContext, nsIFrame* aFrame,
                                    StyleAppearance aAppearance,
                                    const nsRect& aRect) {
  QStyle* style = QApplication::style();
  QWidget* host = StylePaintHost();
  if (!style) {
    return false;
  }

  nsPresContext* pc = aFrame->PresContext();
  const nscoord appUnitsPerDevPixel = pc->AppUnitsPerDevPixel();

  gfxRect gfxRectDev = pc->AppUnitsToGfxUnits(aRect);
  bool snapped = aContext->UserToDevicePixelSnapped(
      gfxRectDev, gfxContext::SnapOption::PrioritizeSize);
  if (!snapped) {
    gfxRectDev = aContext->UserToDevice(gfxRectDev);
  }

  const int width = std::max(1, NS_lround(gfxRectDev.Width()));
  const int height = std::max(1, NS_lround(gfxRectDev.Height()));
  if (width > 4096 || height > 4096) {
    return false;
  }

  if (host) {
    host->resize(width, height);
  }

  QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
  if (image.isNull()) {
    return false;
  }
  image.fill(Qt::transparent);

  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

  const dom::ElementState elementState = GetContentState(aFrame, aAppearance);
  const QStyle::State qstate = ElementStateToQtState(elementState, aAppearance);
  const QRect widgetRect(0, 0, width, height);

  switch (aAppearance) {
    case StyleAppearance::Button:
    case StyleAppearance::Toolbarbutton: {
      QStyleOptionButton opt;
      InitCommonOption(opt, host, widgetRect, qstate | QStyle::State_Raised);
      if (IsDefaultButton(aFrame)) {
        opt.features |= QStyleOptionButton::DefaultButton;
      }
      if (elementState.HasState(dom::ElementState::ACTIVE)) {
        opt.state |= QStyle::State_Sunken;
        opt.state &= ~QStyle::State_Raised;
      }
      style->drawControl(QStyle::CE_PushButtonBevel, &opt, &painter, host);
      style->drawControl(QStyle::CE_PushButton, &opt, &painter, host);
      if (!ImageHasVisiblePixels(image)) {
        image.fill(Qt::transparent);
        style->drawControl(QStyle::CE_PushButton, &opt, &painter, host);
      }
      break;
    }
    case StyleAppearance::Textfield:
    case StyleAppearance::Searchfield:
    case StyleAppearance::NumberInput:
    case StyleAppearance::PasswordInput:
    case StyleAppearance::Textarea: {
      QStyleOptionFrame opt;
      InitCommonOption(opt, host, widgetRect, qstate | QStyle::State_Sunken);
      opt.lineWidth = style->pixelMetric(QStyle::PM_DefaultFrameWidth, &opt, host);
      opt.midLineWidth = 0;
      opt.features = QStyleOptionFrame::None;
      style->drawPrimitive(QStyle::PE_PanelLineEdit, &opt, &painter, host);
      style->drawPrimitive(QStyle::PE_FrameLineEdit, &opt, &painter, host);
      break;
    }
    case StyleAppearance::Menupopup: {
      QStyleOptionFrame opt;
      InitCommonOption(opt, host, widgetRect, qstate | QStyle::State_Raised);
      opt.lineWidth = style->pixelMetric(QStyle::PM_MenuBarPanelWidth, &opt, host);
      if (opt.lineWidth <= 0) {
        opt.lineWidth =
            style->pixelMetric(QStyle::PM_DefaultFrameWidth, &opt, host);
      }
      style->drawPrimitive(QStyle::PE_PanelMenu, &opt, &painter, host);
      style->drawPrimitive(QStyle::PE_FrameMenu, &opt, &painter, host);
      break;
    }
    case StyleAppearance::Listbox: {
      QStyleOptionFrame opt;
      InitCommonOption(opt, host, widgetRect, qstate | QStyle::State_Sunken);
      opt.lineWidth = style->pixelMetric(QStyle::PM_DefaultFrameWidth, &opt, host);
      style->drawPrimitive(QStyle::PE_Frame, &opt, &painter, host);
      style->drawPrimitive(QStyle::PE_PanelLineEdit, &opt, &painter, host);
      break;
    }
    case StyleAppearance::Menulist:
    case StyleAppearance::MenulistButton: {
      QStyleOptionComboBox opt;
      InitCommonOption(opt, host, widgetRect, qstate);
      opt.editable = false;
      opt.frame = true;
      opt.subControls = QStyle::SC_ComboBoxFrame | QStyle::SC_ComboBoxArrow |
                        QStyle::SC_ComboBoxEditField;
      if (IsOpenButton(aFrame)) {
        opt.state |= QStyle::State_On;
      }
      style->drawComplexControl(QStyle::CC_ComboBox, &opt, &painter, host);
      break;
    }
    case StyleAppearance::MozMenulistArrowButton: {
      QStyleOptionComboBox opt;
      InitCommonOption(opt, host, widgetRect, qstate);
      opt.subControls = QStyle::SC_ComboBoxArrow;
      if (IsOpenButton(aFrame)) {
        opt.state |= QStyle::State_On;
      }
      style->drawComplexControl(QStyle::CC_ComboBox, &opt, &painter, host);
      break;
    }
    case StyleAppearance::ProgressBar:
    case StyleAppearance::Meter: {
      QStyleOptionProgressBar opt;
      InitCommonOption(opt, host, widgetRect, qstate | QStyle::State_Horizontal);
      opt.minimum = 0;
      opt.maximum = 100;
      double maxVal = GetProgressMaxValue(aFrame);
      double val = GetProgressValue(aFrame);
      if (maxVal <= 0) {
        opt.maximum = 0;
        opt.progress = 0;
      } else {
        opt.progress = int(std::clamp(val / maxVal, 0.0, 1.0) * 100.0);
      }
      opt.textVisible = false;
      // Breeze paints groove/contents as separate primitives more reliably.
      style->drawControl(QStyle::CE_ProgressBarGroove, &opt, &painter, host);
      style->drawControl(QStyle::CE_ProgressBarContents, &opt, &painter, host);
      if (!ImageHasVisiblePixels(image)) {
        image.fill(Qt::transparent);
        style->drawControl(QStyle::CE_ProgressBar, &opt, &painter, host);
      }
      break;
    }
    case StyleAppearance::Range: {
      QStyleOptionSlider opt;
      InitCommonOption(opt, host, widgetRect, qstate);
      const bool horizontal = IsRangeHorizontal(aFrame);
      opt.orientation = horizontal ? Qt::Horizontal : Qt::Vertical;
      if (horizontal) {
        opt.state |= QStyle::State_Horizontal;
      }
      opt.minimum = 0;
      opt.maximum = 1000;
      opt.sliderValue = 500;
      opt.sliderPosition = 500;
      if (aFrame && aFrame->GetContent()) {
        if (auto* input = mozilla::dom::HTMLInputElement::FromNode(
                aFrame->GetContent())) {
          const double value = input->ValueAsNumber();
          if (std::isfinite(value)) {
            const double min = input->GetMinimum().toDouble();
            const double max = input->GetMaximum().toDouble();
            if (std::isfinite(min) && std::isfinite(max) && max > min) {
              opt.sliderValue =
                  int(std::clamp((value - min) / (max - min), 0.0, 1.0) *
                      1000.0);
              opt.sliderPosition = opt.sliderValue;
            }
          }
        }
      }
      opt.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
      opt.activeSubControls = QStyle::SC_None;
      if (elementState.HasState(dom::ElementState::ACTIVE) ||
          elementState.HasState(dom::ElementState::HOVER)) {
        opt.activeSubControls = QStyle::SC_SliderHandle;
      }
      style->drawComplexControl(QStyle::CC_Slider, &opt, &painter, host);
      break;
    }
    case StyleAppearance::SpinnerUpbutton:
    case StyleAppearance::SpinnerDownbutton: {
      QStyleOptionSpinBox opt;
      InitCommonOption(opt, host, widgetRect, qstate);
      opt.frame = false;
      opt.buttonSymbols = QAbstractSpinBox::UpDownArrows;
      opt.stepEnabled = QAbstractSpinBox::StepUpEnabled |
                        QAbstractSpinBox::StepDownEnabled;
      if (aAppearance == StyleAppearance::SpinnerUpbutton) {
        opt.subControls = QStyle::SC_SpinBoxUp;
        if (elementState.HasState(dom::ElementState::ACTIVE)) {
          opt.activeSubControls = QStyle::SC_SpinBoxUp;
        }
      } else {
        opt.subControls = QStyle::SC_SpinBoxDown;
        if (elementState.HasState(dom::ElementState::ACTIVE)) {
          opt.activeSubControls = QStyle::SC_SpinBoxDown;
        }
      }
      style->drawComplexControl(QStyle::CC_SpinBox, &opt, &painter, host);
      break;
    }
    case StyleAppearance::Checkbox:
    case StyleAppearance::Radio: {
      QStyleOptionButton opt;
      InitCommonOption(opt, host, widgetRect, qstate);
      if (elementState.HasState(dom::ElementState::CHECKED)) {
        opt.state |= QStyle::State_On;
        opt.state &= ~QStyle::State_Off;
      } else {
        opt.state |= QStyle::State_Off;
        opt.state &= ~QStyle::State_On;
      }
      if (elementState.HasState(dom::ElementState::INDETERMINATE) &&
          aAppearance == StyleAppearance::Checkbox) {
        opt.state |= QStyle::State_NoChange;
      }
      // Base plate: many styles sample the widget background under the mark.
      painter.fillRect(widgetRect, opt.palette.color(QPalette::Base));

      const QStyle::ControlElement ce = aAppearance == StyleAppearance::Checkbox
                                            ? QStyle::CE_CheckBox
                                            : QStyle::CE_RadioButton;
      const QStyle::PrimitiveElement pe =
          aAppearance == StyleAppearance::Checkbox
              ? QStyle::PE_IndicatorCheckBox
              : QStyle::PE_IndicatorRadioButton;
      const QStyle::SubElement se = aAppearance == StyleAppearance::Checkbox
                                        ? QStyle::SE_CheckBoxIndicator
                                        : QStyle::SE_RadioButtonIndicator;

      style->drawControl(ce, &opt, &painter, host);
      if (!ImageHasVisiblePixels(image)) {
        image.fill(Qt::transparent);
        painter.fillRect(widgetRect, opt.palette.color(QPalette::Base));
        QRect indicator = style->subElementRect(se, &opt, host);
        if (!indicator.isValid() || indicator.isEmpty()) {
          indicator = widgetRect;
        }
        opt.rect = indicator;
        style->drawPrimitive(pe, &opt, &painter, host);
        if (!ImageHasVisiblePixels(image)) {
          return false;
        }
      }
      break;
    }
    case StyleAppearance::Tooltip: {
      QStyleOptionFrame opt;
      InitCommonOption(opt, host, widgetRect, qstate | QStyle::State_Raised);
      opt.lineWidth =
          style->pixelMetric(QStyle::PM_ToolTipLabelFrameWidth, &opt, host);
      style->drawPrimitive(QStyle::PE_PanelTipLabel, &opt, &painter, host);
      break;
    }
    case StyleAppearance::FocusOutline: {
      QStyleOptionFocusRect opt;
      InitCommonOption(opt, host, widgetRect,
                       qstate | QStyle::State_KeyboardFocusChange |
                           QStyle::State_HasFocus);
      style->drawPrimitive(QStyle::PE_FrameFocusRect, &opt, &painter, host);
      break;
    }
    case StyleAppearance::ScrollbarVertical:
    case StyleAppearance::ScrollbarHorizontal: {
      QStyleOptionSlider opt;
      InitCommonOption(opt, host, widgetRect, qstate);
      const bool horizontal =
          aAppearance == StyleAppearance::ScrollbarHorizontal;
      opt.orientation = horizontal ? Qt::Horizontal : Qt::Vertical;
      if (horizontal) {
        opt.state |= QStyle::State_Horizontal;
      }
      opt.minimum = 0;
      opt.maximum = 1000;
      opt.pageStep = 200;
      opt.singleStep = 20;
      opt.sliderValue = 0;
      opt.sliderPosition = 0;
      opt.subControls = QStyle::SC_ScrollBarGroove | QStyle::SC_ScrollBarAddPage |
                        QStyle::SC_ScrollBarSubPage;
      style->drawComplexControl(QStyle::CC_ScrollBar, &opt, &painter, host);
      break;
    }
    case StyleAppearance::ScrollbarthumbVertical:
    case StyleAppearance::ScrollbarthumbHorizontal: {
      QStyleOptionSlider opt;
      InitCommonOption(opt, host, widgetRect, qstate);
      const bool horizontal =
          aAppearance == StyleAppearance::ScrollbarthumbHorizontal;
      opt.orientation = horizontal ? Qt::Horizontal : Qt::Vertical;
      if (horizontal) {
        opt.state |= QStyle::State_Horizontal;
      }
      opt.minimum = 0;
      opt.maximum = 1000;
      opt.pageStep = 200;
      opt.sliderValue = 500;
      opt.sliderPosition = 500;
      opt.subControls = QStyle::SC_ScrollBarSlider;
      opt.activeSubControls = QStyle::SC_ScrollBarSlider;
      style->drawComplexControl(QStyle::CC_ScrollBar, &opt, &painter, host);
      break;
    }
    case StyleAppearance::ScrollbarbuttonUp:
    case StyleAppearance::ScrollbarbuttonDown:
    case StyleAppearance::ScrollbarbuttonLeft:
    case StyleAppearance::ScrollbarbuttonRight: {
      QStyleOptionSlider opt;
      InitCommonOption(opt, host, widgetRect, qstate);
      const bool horizontal =
          aAppearance == StyleAppearance::ScrollbarbuttonLeft ||
          aAppearance == StyleAppearance::ScrollbarbuttonRight;
      opt.orientation = horizontal ? Qt::Horizontal : Qt::Vertical;
      if (horizontal) {
        opt.state |= QStyle::State_Horizontal;
      }
      if (aAppearance == StyleAppearance::ScrollbarbuttonUp ||
          aAppearance == StyleAppearance::ScrollbarbuttonLeft) {
        opt.subControls = QStyle::SC_ScrollBarSubLine;
        opt.activeSubControls = QStyle::SC_ScrollBarSubLine;
      } else {
        opt.subControls = QStyle::SC_ScrollBarAddLine;
        opt.activeSubControls = QStyle::SC_ScrollBarAddLine;
      }
      style->drawComplexControl(QStyle::CC_ScrollBar, &opt, &painter, host);
      break;
    }
    case StyleAppearance::Scrollcorner: {
      QStyleOption opt;
      InitCommonOption(opt, host, widgetRect, qstate);
      style->drawPrimitive(QStyle::PE_PanelScrollAreaCorner, &opt, &painter,
                           host);
      break;
    }
    default:
      return false;
  }

  painter.end();

  if (!ImageHasVisiblePixels(image)) {
    return false;
  }

  DrawImageToContext(aContext, image, gfxRectDev.TopLeft(), snapped);
  (void)appUnitsPerDevPixel;
  return true;
}

// Theme centers checkbox/radio glyphs in the paint rect. Prefer a square at
// the inline-start edge so flex-stretched XUL frames do not shift the mark.
static nsRect CheckboxRadioPaintRect(nsIFrame* aFrame, const nsRect& aRect) {
  nscoord side = std::min(aRect.width, aRect.height);
  if (side <= 0) {
    return aRect;
  }
  nsRect r = aRect;
  r.width = side;
  r.height = side;
  r.y += (aRect.height - side) / 2;
  if (aFrame && aFrame->GetWritingMode().IsPhysicalRTL()) {
    r.x = aRect.XMost() - side;
  }
  return r;
}

void nsNativeThemeQt::DrawWidgetBackground(
    gfxContext* aContext, nsIFrame* aFrame, StyleAppearance aAppearance,
    const nsRect& aRect, const nsRect& aDirtyRect, DrawOverflow aDrawOverflow) {
  const nsRect paintRect =
      (aAppearance == StyleAppearance::Checkbox ||
       aAppearance == StyleAppearance::Radio)
          ? CheckboxRadioPaintRect(aFrame, aRect)
          : aRect;
  if (ChoosePaintMode(aFrame, aAppearance) == PaintMode::QtStyle) {
    if (PaintQtWidget(aContext, aFrame, aAppearance, paintRect)) {
      return;
    }
  }
  Theme::DrawWidgetBackground(aContext, aFrame, aAppearance, paintRect,
                              aDirtyRect, aDrawOverflow);
}

bool nsNativeThemeQt::CreateWebRenderCommandsForWidget(
    mozilla::wr::DisplayListBuilder& aBuilder,
    mozilla::wr::IpcResourceUpdateQueue& aResources,
    const mozilla::layers::StackingContextHelper& aSc,
    mozilla::layers::RenderRootStateManager* aManager, nsIFrame* aFrame,
    StyleAppearance aAppearance, const nsRect& aRect) {
  // QStyle rasterizes to a bitmap; fall back to DrawWidgetBackground so WR
  // records a blob image / software fallback.
  if (ChoosePaintMode(aFrame, aAppearance) == PaintMode::QtStyle) {
    return false;
  }
  if (aAppearance == StyleAppearance::Checkbox ||
      aAppearance == StyleAppearance::Radio) {
    const nsRect paintRect = CheckboxRadioPaintRect(aFrame, aRect);
    return Theme::CreateWebRenderCommandsForWidget(
        aBuilder, aResources, aSc, aManager, aFrame, aAppearance, paintRect);
  }
  return Theme::CreateWebRenderCommandsForWidget(
      aBuilder, aResources, aSc, aManager, aFrame, aAppearance, aRect);
}

LayoutDeviceIntMargin nsNativeThemeQt::GetWidgetBorder(
    nsDeviceContext* aContext, nsIFrame* aFrame, StyleAppearance aAppearance) {
  if (ChoosePaintMode(aFrame, aAppearance) != PaintMode::QtStyle) {
    return Theme::GetWidgetBorder(aContext, aFrame, aAppearance);
  }

  QStyle* style = QApplication::style();
  QWidget* host = StylePaintHost();
  if (!style) {
    return Theme::GetWidgetBorder(aContext, aFrame, aAppearance);
  }

  LayoutDeviceIntMargin result;
  switch (aAppearance) {
    case StyleAppearance::Button:
    case StyleAppearance::Toolbarbutton: {
      QStyleOptionButton opt;
      InitCommonOption(opt, host, QRect(0, 0, 100, 30), QStyle::State_Enabled);
      // contentsRect inset approximates the frame chrome.
      const QRect outer(0, 0, 100, 30);
      const QRect inner =
          style->subElementRect(QStyle::SE_PushButtonContents, &opt, host);
      if (inner.isValid() && !inner.isEmpty() && inner != outer) {
        result.SizeTo(inner.left() - outer.left(), inner.top() - outer.top(),
                      outer.right() - inner.right(),
                      outer.bottom() - inner.bottom());
      } else {
        int fw = style->pixelMetric(QStyle::PM_ButtonMargin, &opt, host);
        result.SizeTo(fw, fw, fw, fw);
      }
      break;
    }
    case StyleAppearance::Textfield:
    case StyleAppearance::Searchfield:
    case StyleAppearance::NumberInput:
    case StyleAppearance::PasswordInput:
    case StyleAppearance::Textarea:
    case StyleAppearance::Listbox:
    case StyleAppearance::Menulist:
    case StyleAppearance::MenulistButton: {
      QStyleOptionFrame opt;
      InitCommonOption(opt, host, QRect(0, 0, 100, 24),
                       QStyle::State_Enabled | QStyle::State_Sunken);
      int fw = style->pixelMetric(QStyle::PM_DefaultFrameWidth, &opt, host);
      result.SizeTo(fw, fw, fw, fw);
      break;
    }
    case StyleAppearance::Menupopup: {
      QStyleOptionFrame opt;
      InitCommonOption(opt, host, QRect(0, 0, 100, 100),
                       QStyle::State_Enabled | QStyle::State_Raised);
      int fw = style->pixelMetric(QStyle::PM_MenuBarPanelWidth, &opt, host);
      if (fw <= 0) {
        fw = style->pixelMetric(QStyle::PM_DefaultFrameWidth, &opt, host);
      }
      result.SizeTo(fw, fw, fw, fw);
      break;
    }
    case StyleAppearance::Tooltip: {
      QStyleOptionFrame opt;
      InitCommonOption(opt, host, QRect(0, 0, 100, 24), QStyle::State_Enabled);
      int fw =
          style->pixelMetric(QStyle::PM_ToolTipLabelFrameWidth, &opt, host);
      if (fw < 0) {
        fw = 1;
      }
      result.SizeTo(fw, fw, fw, fw);
      break;
    }
    default:
      break;
  }
  // Clamp nonsense from styles that return huge insets without a real widget.
  auto clampSide = [](auto v) {
    const int i = int(v);
    return std::clamp(i, 0, 32);
  };
  result.top = clampSide(result.top);
  result.right = clampSide(result.right);
  result.bottom = clampSide(result.bottom);
  result.left = clampSide(result.left);
  return result;
}

bool nsNativeThemeQt::GetWidgetPadding(nsDeviceContext* aContext,
                                       nsIFrame* aFrame,
                                       StyleAppearance aAppearance,
                                       LayoutDeviceIntMargin* aResult) {
  if (ChoosePaintMode(aFrame, aAppearance) != PaintMode::QtStyle) {
    return Theme::GetWidgetPadding(aContext, aFrame, aAppearance, aResult);
  }

  // Checkbox/radio: zero padding so the indicator fills the themed box.
  if (aAppearance == StyleAppearance::Checkbox ||
      aAppearance == StyleAppearance::Radio) {
    aResult->SizeTo(0, 0, 0, 0);
    return true;
  }

  // Let CSS padding apply for text-like controls.
  return false;
}

LayoutDeviceIntSize nsNativeThemeQt::GetMinimumWidgetSize(
    nsPresContext* aPresContext, nsIFrame* aFrame,
    StyleAppearance aAppearance) {
  QStyle* style = QApplication::style();
  QWidget* host = StylePaintHost();

  if (aAppearance == StyleAppearance::Checkbox ||
      aAppearance == StyleAppearance::Radio) {
    if (style) {
      QStyleOptionButton opt;
      InitCommonOption(opt, host, QRect(0, 0, 16, 16), QStyle::State_Enabled);
      const QStyle::ContentsType ct =
          aAppearance == StyleAppearance::Checkbox ? QStyle::CT_CheckBox
                                                   : QStyle::CT_RadioButton;
      const QSize sz = style->sizeFromContents(ct, &opt, QSize(0, 0), host);
      if (sz.width() > 0 && sz.height() > 0) {
        LayoutDeviceIntSize result;
        result.SizeTo(sz.width(), sz.height());
        return result;
      }
      const QStyle::PixelMetric metricW =
          aAppearance == StyleAppearance::Checkbox
              ? QStyle::PM_IndicatorWidth
              : QStyle::PM_ExclusiveIndicatorWidth;
      const QStyle::PixelMetric metricH =
          aAppearance == StyleAppearance::Checkbox
              ? QStyle::PM_IndicatorHeight
              : QStyle::PM_ExclusiveIndicatorHeight;
      int w = style->pixelMetric(metricW, &opt, host);
      int h = style->pixelMetric(metricH, &opt, host);
      if (w > 0 && h > 0) {
        LayoutDeviceIntSize result;
        result.SizeTo(w, h);
        return result;
      }
    }
    const DPIRatio dpiRatio = GetDPIRatio(aFrame, aAppearance);
    const CSSCoord side = GetCheckboxRadioPrefSize();
    LayoutDeviceIntSize result;
    result.SizeTo((side * dpiRatio).Rounded(), (side * dpiRatio).Rounded());
    return result;
  }

  if (ChoosePaintMode(aFrame, aAppearance) != PaintMode::QtStyle) {
    return Theme::GetMinimumWidgetSize(aPresContext, aFrame, aAppearance);
  }

  if (!style) {
    return Theme::GetMinimumWidgetSize(aPresContext, aFrame, aAppearance);
  }

  LayoutDeviceIntSize result;

  switch (aAppearance) {
    case StyleAppearance::Button:
    case StyleAppearance::Toolbarbutton: {
      QStyleOptionButton opt;
      InitCommonOption(opt, host, QRect(0, 0, 0, 0), QStyle::State_Enabled);
      const QSize sz =
          style->sizeFromContents(QStyle::CT_PushButton, &opt, QSize(16, 8), host);
      if (sz.height() > 0) {
        result.height = sz.height();
      }
      break;
    }
    case StyleAppearance::Menulist:
    case StyleAppearance::MenulistButton: {
      QStyleOptionComboBox opt;
      InitCommonOption(opt, host, QRect(0, 0, 0, 0), QStyle::State_Enabled);
      const QSize sz =
          style->sizeFromContents(QStyle::CT_ComboBox, &opt, QSize(40, 16), host);
      if (sz.height() > 0) {
        result.height = sz.height();
      }
      int w = style->pixelMetric(QStyle::PM_MenuButtonIndicator, &opt, host);
      if (w > 0) {
        result.width = std::max(result.width, w + 24);
      }
      break;
    }
    case StyleAppearance::MozMenulistArrowButton: {
      int w = style->pixelMetric(QStyle::PM_MenuButtonIndicator, nullptr, host);
      if (w <= 0) {
        w = 16;
      }
      result.width = w;
      break;
    }
    case StyleAppearance::Range: {
      QStyleOptionSlider opt;
      InitCommonOption(opt, host, QRect(0, 0, 100, 20),
                       QStyle::State_Enabled | QStyle::State_Horizontal);
      opt.orientation = Qt::Horizontal;
      const QSize sz =
          style->sizeFromContents(QStyle::CT_Slider, &opt, QSize(100, 16), host);
      if (sz.height() > 0) {
        result.height = sz.height();
      }
      int tw = style->pixelMetric(QStyle::PM_SliderThickness, &opt, host);
      if (tw > 0) {
        result.height = std::max(result.height, tw);
      }
      break;
    }
    case StyleAppearance::SpinnerUpbutton:
    case StyleAppearance::SpinnerDownbutton: {
      int w = style->pixelMetric(QStyle::PM_SpinBoxSliderHeight, nullptr, host);
      if (w <= 0) {
        w = 16;
      }
      result.SizeTo(w, w);
      break;
    }
    case StyleAppearance::ScrollbarthumbVertical:
    case StyleAppearance::ScrollbarthumbHorizontal: {
      const bool horizontal =
          aAppearance == StyleAppearance::ScrollbarthumbHorizontal;
      QStyleOptionSlider opt;
      InitCommonOption(opt, host, QRect(0, 0, 100, 100), QStyle::State_Enabled);
      opt.orientation = horizontal ? Qt::Horizontal : Qt::Vertical;
      if (horizontal) {
        opt.state |= QStyle::State_Horizontal;
      }
      int thickness =
          style->pixelMetric(QStyle::PM_ScrollBarExtent, &opt, host);
      if (thickness <= 0) {
        thickness = 12;
      }
      if (horizontal) {
        result.height = thickness;
        result.width = thickness * 2;
      } else {
        result.width = thickness;
        result.height = thickness * 2;
      }
      break;
    }
    case StyleAppearance::ScrollbarbuttonUp:
    case StyleAppearance::ScrollbarbuttonDown:
    case StyleAppearance::ScrollbarbuttonLeft:
    case StyleAppearance::ScrollbarbuttonRight: {
      int extent =
          style->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, host);
      if (extent <= 0) {
        extent = 12;
      }
      result.SizeTo(extent, extent);
      break;
    }
    default:
      result = Theme::GetMinimumWidgetSize(aPresContext, aFrame, aAppearance);
      break;
  }
  return result;
}

bool nsNativeThemeQt::ThemeSupportsWidget(nsPresContext* aPresContext,
                                          nsIFrame* aFrame,
                                          StyleAppearance aAppearance) {
  if (PrefNativeThemeControls() && CanPaintWithQtStyle(aAppearance) &&
      !IsWidgetStyled(aPresContext, aFrame, aAppearance)) {
    return true;
  }
  return Theme::ThemeSupportsWidget(aPresContext, aFrame, aAppearance);
}

bool nsNativeThemeQt::ThemeDrawsFocusForWidget(nsIFrame* aFrame,
                                               StyleAppearance aAppearance) {
  if (ChoosePaintMode(aFrame, aAppearance) == PaintMode::QtStyle) {
    return true;
  }
  return Theme::ThemeDrawsFocusForWidget(aFrame, aAppearance);
}

nsITheme::Transparency nsNativeThemeQt::GetWidgetTransparency(
    nsIFrame* aFrame, StyleAppearance aAppearance) {
  if (ChoosePaintMode(aFrame, aAppearance) == PaintMode::QtStyle) {
    return eTransparent;
  }
  return Theme::GetWidgetTransparency(aFrame, aAppearance);
}

already_AddRefed<Theme> do_CreateNativeThemeDoNotUseDirectly() {
  if (gfxPlatform::IsHeadless()) {
    return do_AddRef(new Theme(Theme::ScrollbarStyle()));
  }
  return do_AddRef(new nsNativeThemeQt());
}
