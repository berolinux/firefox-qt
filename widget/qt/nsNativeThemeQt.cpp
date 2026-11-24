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
#include <QPainterPath>
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
  // Match GTK: checkbox/radio always use the shared Theme painter. That path
  // is snap-correct under WebRender blob images and is what paints the
  // commonDialog "remember password" indicator. QStyle PE_Indicator* is also
  // unreliable off-screen under Breeze.
  // Tooltips: Theme fill uses InfoBackground which we pair with InfoText from
  // the system tip palette. PE_PanelTipLabel alone left light-scheme black
  // text on a dark Breeze tip (black-on-black).
  if (aAppearance == StyleAppearance::Checkbox ||
      aAppearance == StyleAppearance::Radio ||
      aAppearance == StyleAppearance::Tooltip ||
      IsWidgetAlwaysNonNative(aFrame, aAppearance)) {
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

  // Moz2D B8G8R8A8 is non-premultiplied; un-premultiply Qt's ARGB32_Premultiplied
  // so alpha-blended edges (and solid glyphs) stay visible instead of washed out.
  QImage converted =
      aImage.convertToFormat(QImage::Format_ARGB32);  // non-premultiplied

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
  // Match nsNativeThemeGTK: when snapped, aDest is device-space; DrawTarget is
  // user-space, so undo the current translation + device offset. Without this,
  // indicators paint outside the clip (invisible checkbox / radio / buttons).
  Point dest;
  if (aSnapped) {
    dest = Point(NS_lround(aDest.x), NS_lround(aDest.y)) -
           dt->GetTransform().GetTranslation() - aContext->GetDeviceOffset();
  } else {
    dest = ToPoint(aDest);
  }
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

  // Start in user space (app units → CSS/gfx units). UserToDevicePixelSnapped
  // may rewrite the rect into device space when it returns true — same as GTK.
  // Do NOT force UserToDevice when unsnapped: DrawTarget paints in user space.
  gfxRect paintRect = pc->AppUnitsToGfxUnits(aRect);
  const bool snapped = aContext->UserToDevicePixelSnapped(
      paintRect, gfxContext::SnapOption::PrioritizeSize);

  const int width = std::max(1, NS_lround(paintRect.Width()));
  const int height = std::max(1, NS_lround(paintRect.Height()));
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
      // Gecko paints the XUL/HTML label; we only paint the indicator box.
      // Breeze/Oxygen PE_Indicator* often emits empty pixels for indicator-only
      // rects (host is off-screen). Filling Base first made ImageHasVisiblePixels
      // always true so our fallback never ran — plain text, no checkbox box.
      // Always draw a simple, high-contrast indicator ourselves.
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
      opt.palette = QApplication::palette();

      const QStyle::PixelMetric metricW =
          aAppearance == StyleAppearance::Checkbox
              ? QStyle::PM_IndicatorWidth
              : QStyle::PM_ExclusiveIndicatorWidth;
      const QStyle::PixelMetric metricH =
          aAppearance == StyleAppearance::Checkbox
              ? QStyle::PM_IndicatorHeight
              : QStyle::PM_ExclusiveIndicatorHeight;
      int iw = style->pixelMetric(metricW, &opt, host);
      int ih = style->pixelMetric(metricH, &opt, host);
      if (iw <= 0) {
        iw = std::min(width, height);
      }
      if (ih <= 0) {
        ih = std::min(width, height);
      }
      // Never request a side larger than the paint rect (old max(12, …) made
      // 1×1/small rects paint empty after intersect + adjusted inset).
      const int side = std::max(1, std::min({iw, ih, width, height}));
      QRect indicator((width - side) / 2, (height - side) / 2, side, side);
      indicator = indicator.intersected(widgetRect);
      if (indicator.isEmpty()) {
        indicator = widgetRect;
      }

      image.fill(Qt::transparent);
      QColor border = opt.palette.color(QPalette::WindowText);
      QColor fill = opt.palette.color(QPalette::Base);
      // Force contrast — Breeze dark can make WindowText ≈ Base when sampled
      // from an off-screen host.
      auto lum = [](const QColor& c) {
        return (2 * c.red() + 3 * c.green() + c.blue()) / 6;
      };
      if (std::abs(lum(border) - lum(fill)) < 48) {
        if (lum(fill) > 128) {
          border = QColor(32, 32, 32);
          fill = QColor(250, 250, 250);
        } else {
          border = QColor(230, 230, 230);
          fill = QColor(40, 40, 40);
        }
      }
      painter.setRenderHint(QPainter::Antialiasing, true);
      const int inset = side >= 6 ? 1 : 0;
      painter.setPen(QPen(border, side >= 8 ? 1.5 : 1.0));
      painter.setBrush(fill);
      if (aAppearance == StyleAppearance::Checkbox) {
        painter.drawRoundedRect(indicator.adjusted(inset, inset, -inset, -inset),
                                2, 2);
        if (opt.state & QStyle::State_On) {
          QPen checkPen(border, std::max(1.5, side / 8.0), Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin);
          painter.setPen(checkPen);
          painter.setBrush(Qt::NoBrush);
          const qreal x = indicator.x();
          const qreal y = indicator.y();
          const qreal w = indicator.width();
          const qreal h = indicator.height();
          QPainterPath path;
          path.moveTo(x + 0.22 * w, y + 0.50 * h);
          path.lineTo(x + 0.42 * w, y + 0.72 * h);
          path.lineTo(x + 0.80 * w, y + 0.28 * h);
          painter.drawPath(path);
        } else if (opt.state & QStyle::State_NoChange) {
          painter.setPen(QPen(border, 2.0, Qt::SolidLine, Qt::RoundCap));
          painter.drawLine(indicator.left() + 3, indicator.center().y(),
                           indicator.right() - 3, indicator.center().y());
        }
      } else {
        painter.drawEllipse(indicator.adjusted(inset, inset, -inset, -inset));
        if (opt.state & QStyle::State_On) {
          painter.setBrush(border);
          painter.setPen(Qt::NoPen);
          const int m = std::max(1, side / 4);
          painter.drawEllipse(indicator.adjusted(m, m, -m, -m));
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

  DrawImageToContext(aContext, image, paintRect.TopLeft(), snapped);
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

  // Checkbox/radio use Theme painting; keep sizes in sync with Theme.
  if (aAppearance == StyleAppearance::Checkbox ||
      aAppearance == StyleAppearance::Radio ||
      ChoosePaintMode(aFrame, aAppearance) != PaintMode::QtStyle) {
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
