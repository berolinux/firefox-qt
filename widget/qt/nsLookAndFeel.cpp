/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsLookAndFeel.h"

#include <algorithm>
#include <cstring>

#include "gfxFont.h"
#include "gfxFontConstants.h"
#include "mozilla/FontPropertyTypes.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/Maybe.h"
#include "QtPrefUtils.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/widget/ThemeChangeKind.h"
#include "nsStyleConsts.h"
#include "nsXPLookAndFeel.h"
#include "prenv.h"

#ifdef MOZ_ENABLE_DBUS
#  include "DBusMenuBar.h"
#  include "PortalUtils.h"
#endif

#pragma GCC visibility push(default)
#include <QApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QInputDevice>
#include <QObject>
#include <QPalette>
#include <QScreen>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QVariant>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>
#include <QtDBus/QDBusVariant>
#pragma GCC visibility pop

using namespace mozilla;
using mozilla::widget::ThemeChangeKind;

namespace {

static nscolor QColorToNscolor(const QColor& aColor) {
  return NS_RGBA(aColor.red(), aColor.green(), aColor.blue(), aColor.alpha());
}

static bool PreferSettingsPortal() {
#ifdef MOZ_ENABLE_DBUS
  return widget::ShouldUsePortal(widget::PortalKind::Settings);
#else
  return false;
#endif
}

// Unwrap nested QDBusVariant shells used by xdg-desktop-portal Settings.Read.
static QVariant UnwrapPortalVariant(const QVariant& aValue) {
  QVariant v = aValue;
  for (int i = 0; i < 4; ++i) {
    if (v.userType() != qMetaTypeId<QDBusVariant>()) {
      break;
    }
    v = v.value<QDBusVariant>().variant();
  }
  return v;
}

// LookAndFeel is queried before nsAppShell::Init. Do not create QApplication
// here (that caused a busy event-loop spin); use stand-ins until Qt is up.
static bool QtAppReady() {
  return QApplication::instance() != nullptr;
}

// Portal Settings.Read is a blocking D-Bus call. It MUST NOT run from
// NativeGetColor / SystemIsDark — those are hit from refresh-driver theme
// fills and were pegging the main thread (~50% CPU, blank canvas, dead input).
struct PortalSettingsCache {
  bool populated = false;
  Maybe<bool> colorSchemeDark;  // Nothing = unknown / no preference
  Maybe<nscolor> accent;
};

static PortalSettingsCache sPortalCache;

static Maybe<QVariant> ReadPortalSettingOnce(const char* aNamespace,
                                             const char* aKey) {
  if (!PreferSettingsPortal() || !QtAppReady()) {
    return Nothing();
  }
  if (QGuiApplication::desktopFileName().isEmpty()) {
    return Nothing();
  }
  // Short timeout: this runs only from RefreshImpl/NativeInit, never per-frame.
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QStringLiteral("org.freedesktop.portal.Desktop"),
      QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.Settings"),
      QStringLiteral("Read"));
  msg << QString::fromUtf8(aNamespace) << QString::fromUtf8(aKey);
  const QDBusMessage reply =
      QDBusConnection::sessionBus().call(msg, QDBus::Block, 100);
  if (reply.type() != QDBusMessage::ReplyMessage ||
      reply.arguments().isEmpty()) {
    return Nothing();
  }
  return Some(UnwrapPortalVariant(reply.arguments().at(0)));
}

static void RefreshPortalSettingsCache() {
  sPortalCache = PortalSettingsCache{};
  sPortalCache.populated = true;

  if (Maybe<QVariant> portal =
          ReadPortalSettingOnce("org.freedesktop.appearance", "color-scheme")) {
    bool ok = false;
    const uint scheme = portal->toUInt(&ok);
    if (ok) {
      // 0 no preference, 1 prefer dark, 2 prefer light.
      if (scheme == 1) {
        sPortalCache.colorSchemeDark = Some(true);
      } else if (scheme == 2) {
        sPortalCache.colorSchemeDark = Some(false);
      }
    }
  }
  if (Maybe<QVariant> portal =
          ReadPortalSettingOnce("org.freedesktop.appearance", "accent-color")) {
    const QVariantList parts = portal->toList();
    if (parts.size() >= 3) {
      const double r = parts[0].toDouble();
      const double g = parts[1].toDouble();
      const double b = parts[2].toDouble();
      if (r >= 0.0 && g >= 0.0 && b >= 0.0) {
        sPortalCache.accent =
            Some(NS_RGBA(uint8_t(std::clamp(r, 0.0, 1.0) * 255.0 + 0.5),
                         uint8_t(std::clamp(g, 0.0, 1.0) * 255.0 + 0.5),
                         uint8_t(std::clamp(b, 0.0, 1.0) * 255.0 + 0.5), 255));
      }
    }
  }
}

// Hot path: no D-Bus. Qt styleHints / palette + optional one-shot cache.
static bool SystemIsDark() {
  if (!QtAppReady()) {
    return false;
  }
  if (QStyleHints* hints = QGuiApplication::styleHints()) {
    switch (hints->colorScheme()) {
      case Qt::ColorScheme::Dark:
        return true;
      case Qt::ColorScheme::Light:
        return false;
      case Qt::ColorScheme::Unknown:
        break;
    }
  }
  if (sPortalCache.populated && sPortalCache.colorSchemeDark) {
    return *sPortalCache.colorSchemeDark;
  }
  const QColor window =
      QApplication::palette().color(QPalette::Active, QPalette::Window);
  return window.lightness() < 128;
}

static Maybe<nscolor> CachedPortalAccentColor() {
  if (sPortalCache.populated) {
    return sPortalCache.accent;
  }
  return Nothing();
}

static QStyle* AppStyle() {
  return QApplication::instance() ? QApplication::style() : nullptr;
}

static int StyleHint(QStyle::StyleHint aHint, int aFallback) {
  if (QStyle* style = AppStyle()) {
    return style->styleHint(aHint);
  }
  return aFallback;
}

static int PixelMetric(QStyle::PixelMetric aMetric, int aFallback) {
  if (QStyle* style = AppStyle()) {
    return style->pixelMetric(aMetric);
  }
  return aFallback;
}

static void FillFontFromQFont(const QFont& aFont, nsString& aFontName,
                              gfxFontStyle& aStyle) {
  const QString family = aFont.family();
  aFontName.Assign(reinterpret_cast<const char16_t*>(family.utf16()),
                   family.size());
  // Gecko expects CSS px (96dpi). Qt pointSizeF is PostScript points (72dpi).
  // Match GTK's pango-points → CSS px conversion so system fonts are not
  // undersized (or, with pixelSize fallbacks, oversized on HiDPI).
  const qreal pt = aFont.pointSizeF();
  if (pt > 0) {
    aStyle.size = double(pt) * (96.0 / 72.0);
  } else {
    const int px = aFont.pixelSize();
    if (px > 0) {
      // pixelSize is device pixels; convert to CSS px via app DPR.
      qreal dpr = 1.0;
      if (QScreen* s = QGuiApplication::primaryScreen()) {
        dpr = s->devicePixelRatio() > 0.0 ? s->devicePixelRatio() : 1.0;
      }
      aStyle.size = double(px) / dpr;
    } else {
      aStyle.size = 16.0;  // ~12pt
    }
  }
  aStyle.style =
      aFont.italic() ? FontSlantStyle::ITALIC : FontSlantStyle::NORMAL;
  // Qt6 font weights are 1–1000 (Normal = 400).
  const int weight = static_cast<int>(aFont.weight());
  aStyle.weight = FontWeight::FromInt(std::clamp(weight, 1, 1000));
  aStyle.stretch = FontStretch::NORMAL;
  aStyle.systemFont = true;
}

// Watches QApplication for real theme changes. Intentionally ignores
// ApplicationPaletteChange / StyleChange: those fire in storms during paint
// and restyle on some Qt styles, which re-entered NotifyChangedAllWindows,
// continuous SWGL presents, and ~150% CPU with a dead UI.
class ThemeChangeWatcher final : public QObject {
 public:
  explicit ThemeChangeWatcher(QObject* aParent = nullptr) : QObject(aParent) {
    mDebounce.setSingleShot(true);
    mDebounce.setInterval(200);
    QObject::connect(&mDebounce, &QTimer::timeout, this, [this]() {
      if (mNotifying) {
        return;
      }
      mNotifying = true;
      LookAndFeel::NotifyChangedAllWindows(ThemeChangeKind::StyleAndLayout);
      mNotifying = false;
    });
  }

  bool eventFilter(QObject* aObj, QEvent* aEvent) override {
    if (mNotifying) {
      return QObject::eventFilter(aObj, aEvent);
    }
    switch (aEvent->type()) {
      case QEvent::ApplicationFontChange:
      case QEvent::ThemeChange:
        if (!mDebounce.isActive()) {
          mDebounce.start();
        }
        break;
      default:
        break;
    }
    return QObject::eventFilter(aObj, aEvent);
  }

 private:
  QTimer mDebounce;
  bool mNotifying = false;
};

}  // namespace

nsLookAndFeel::nsLookAndFeel() = default;

nsLookAndFeel::~nsLookAndFeel() {
  if (mThemeWatcher) {
    auto* watcher = static_cast<ThemeChangeWatcher*>(mThemeWatcher);
    mThemeWatcher = nullptr;
    if (QApplication::instance()) {
      qApp->removeEventFilter(watcher);
    }
    // Unparented QObject — delete immediately; do not deleteLater (QApp may
    // already be tearing down when module dtor runs).
    delete watcher;
  }
}

void nsLookAndFeel::EnsureThemeWatcher() {
  if (mThemeWatcher || !QApplication::instance()) {
    return;
  }
  // Intentionally unparented: we own the watcher and delete it in our
  // destructor / Shutdown before QApplication goes away.
  auto* watcher = new ThemeChangeWatcher(/* parent = */ nullptr);
  qApp->installEventFilter(watcher);
  mThemeWatcher = watcher;
}

void nsLookAndFeel::NativeInit() {
  EnsureThemeWatcher();
  RefreshPortalSettingsCache();
  RecordTelemetry();
}

void nsLookAndFeel::RefreshImpl() {
  EnsureThemeWatcher();
  RefreshPortalSettingsCache();
  nsXPLookAndFeel::RefreshImpl();
}

nsresult nsLookAndFeel::NativeGetColor(ColorID aID, ColorScheme aScheme,
                                       nscolor& aResult) {
  if (!QtAppReady()) {
    aResult = GetStandinForNativeColor(aID, aScheme);
    return NS_OK;
  }

  const QPalette pal = QApplication::palette();
  auto color = [&](QPalette::ColorRole role,
                   QPalette::ColorGroup group = QPalette::Active) {
    return QColorToNscolor(pal.color(group, role));
  };

  // Tooltips: always pair with the *system* tip palette (Breeze/Oxygen paint
  // PE_PanelTipLabel from QPalette, not document color-scheme). Light-scheme
  // standin InfoText (black) over a dark system tip is unreadable black-on-
  // black; resolve both colors together before the scheme mismatch early-out.
  if (aID == ColorID::Infobackground || aID == ColorID::Infotext) {
    nscolor bg = color(QPalette::ToolTipBase);
    nscolor fg = color(QPalette::ToolTipText);
    auto lum = [](nscolor c) {
      return (2 * NS_GET_R(c) + 3 * NS_GET_G(c) + NS_GET_B(c)) / 6;
    };
    if (std::abs(int(lum(bg)) - int(lum(fg))) < 80) {
      // Pathological palette (or monochrome tip roles): force readable pair.
      if (SystemIsDark() || lum(bg) < 128) {
        bg = NS_RGB(0x2b, 0x2a, 0x33);
        fg = NS_RGB(0xfb, 0xfb, 0xfe);
      } else {
        bg = NS_RGB(0xf9, 0xf9, 0xfb);
        fg = NS_RGB(0x15, 0x15, 0x1a);
      }
    }
    aResult = (aID == ColorID::Infobackground) ? bg : fg;
    return NS_OK;
  }

  // Qt only exposes the active system palette. For the opposite scheme, fall
  // back to standins so light chrome is still usable on a dark desktop and
  // vice versa.
  if ((aScheme == ColorScheme::Dark) != SystemIsDark()) {
    aResult = GetStandinForNativeColor(aID, aScheme);
    return NS_OK;
  }

  // Slightly adjust a color toward another for hover/active variants when the
  // style does not expose a dedicated role.
  auto mix = [](nscolor a, nscolor b, float t) {
    auto ch = [&](int shift) {
      const float ca = (a >> shift) & 0xff;
      const float cb = (b >> shift) & 0xff;
      return uint8_t(ca * (1.0f - t) + cb * t + 0.5f);
    };
    return NS_RGBA(ch(16), ch(8), ch(0), 255);
  };

  const nscolor window = color(QPalette::Window);
  const nscolor windowText = color(QPalette::WindowText);
  const nscolor base = color(QPalette::Base);
  const nscolor text = color(QPalette::Text);
  const nscolor button = color(QPalette::Button);
  const nscolor buttonText = color(QPalette::ButtonText);
  const nscolor highlight = color(QPalette::Highlight);
  const nscolor highlightText = color(QPalette::HighlightedText);
  const nscolor mid = color(QPalette::Mid);
  const nscolor midlight = color(QPalette::Midlight);
  const nscolor light = color(QPalette::Light);
  const nscolor dark = color(QPalette::Dark);
  const nscolor shadow = color(QPalette::Shadow);
  const nscolor altBase = color(QPalette::AlternateBase);
  const nscolor link = color(QPalette::Link);
  const nscolor linkVisited = color(QPalette::LinkVisited);
  nscolor accent = color(QPalette::Accent);
  if (Maybe<nscolor> portalAccent = CachedPortalAccentColor()) {
    accent = *portalAccent;
  }
  const nscolor accentText = highlightText;
  const nscolor disabledText = color(QPalette::WindowText, QPalette::Disabled);
  const nscolor disabledButton = color(QPalette::Button, QPalette::Disabled);

  switch (aID) {
    case ColorID::Window:
    case ColorID::Windowframe:
    case ColorID::Appworkspace:
    case ColorID::Background:
    case ColorID::MozDialog:
    case ColorID::MozSidebar:
    case ColorID::MozHeaderbar:
    case ColorID::MozHeaderbarinactive:
    case ColorID::MozColheader:
    case ColorID::Activecaption:
    case ColorID::Inactivecaption:
      aResult = window;
      return NS_OK;

    case ColorID::Windowtext:
    case ColorID::MozDialogtext:
    case ColorID::MozSidebartext:
    case ColorID::MozHeaderbartext:
    case ColorID::MozHeaderbarinactivetext:
    case ColorID::Captiontext:
    case ColorID::Inactivecaptiontext:
    case ColorID::MozColheadertext:
      aResult = windowText;
      return NS_OK;

    case ColorID::Buttonface:
    case ColorID::Threedface:
    case ColorID::MozButtonhoverface:
    case ColorID::MozButtonactiveface:
    case ColorID::MozButtondisabledface:
    case ColorID::MozColheaderhover:
    case ColorID::MozColheaderactive:
      if (aID == ColorID::MozButtondisabledface) {
        aResult = disabledButton;
      } else if (aID == ColorID::MozButtonhoverface) {
        aResult = midlight;
      } else if (aID == ColorID::MozButtonactiveface) {
        aResult = mid;
      } else {
        aResult = button;
      }
      return NS_OK;

    case ColorID::Buttontext:
    case ColorID::MozButtonhovertext:
    case ColorID::MozButtonactivetext:
    case ColorID::MozColheaderhovertext:
    case ColorID::MozColheaderactivetext:
      aResult = buttonText;
      return NS_OK;

    case ColorID::MozButtonhoverborder:
    case ColorID::MozButtonactiveborder:
    case ColorID::Buttonborder: {
      // QPalette::Dark is often nearly identical to Base/Window on Breeze
      // dark, which makes unselected radio/checkbox rings vanish (Theme
      // collapses border when fill==border). Ensure a visible outline.
      nscolor candidate = (aID == ColorID::MozButtonhoverborder) ? midlight
                          : (aID == ColorID::MozButtonactiveborder) ? mid
                                                                    : dark;
      auto luminance = [](nscolor c) {
        return (2 * NS_GET_R(c) + 3 * NS_GET_G(c) + NS_GET_B(c)) / 6;
      };
      const int contrast = std::abs(int(luminance(candidate)) -
                                    int(luminance(base)));
      if (contrast < 48) {
        aResult = mix(windowText, base, 0.45f);
      } else {
        aResult = candidate;
      }
      return NS_OK;
    }

    case ColorID::Buttonhighlight:
    case ColorID::Threedhighlight:
    case ColorID::Threedlightshadow:
      aResult = light;
      return NS_OK;

    case ColorID::Buttonshadow:
    case ColorID::Threedshadow:
      aResult = dark;
      return NS_OK;

    case ColorID::Threeddarkshadow:
    case ColorID::MozSidebarborder:
    case ColorID::Activeborder:
    case ColorID::Inactiveborder:
    case ColorID::MozButtondisabledborder: {
      nscolor candidate = shadow;
      auto luminance = [](nscolor c) {
        return (2 * NS_GET_R(c) + 3 * NS_GET_G(c) + NS_GET_B(c)) / 6;
      };
      if (std::abs(int(luminance(candidate)) - int(luminance(base))) < 40) {
        aResult = mix(windowText, base, 0.30f);
      } else {
        aResult = candidate;
      }
      return NS_OK;
    }

    case ColorID::Field:
    case ColorID::MozCombobox:
      aResult = base;
      return NS_OK;

    case ColorID::MozDisabledfield:
      // Slightly distinct from Field so disabled unchecked radios keep a ring
      // (Theme skips the stroke when fill and border match).
      aResult = mix(base, window, 0.35f);
      return NS_OK;

    case ColorID::Fieldtext:
    case ColorID::MozComboboxtext:
      aResult = text;
      return NS_OK;

    case ColorID::Highlight:
    case ColorID::Selecteditem:
    case ColorID::IMESelectedRawTextBackground:
    case ColorID::IMESelectedConvertedTextBackground:
    case ColorID::MozCellhighlight:
    case ColorID::MozMenuhover:
    case ColorID::TargetTextBackground:
      aResult = highlight;
      return NS_OK;

    case ColorID::Highlighttext:
    case ColorID::Selecteditemtext:
    case ColorID::IMESelectedRawTextForeground:
    case ColorID::IMESelectedConvertedTextForeground:
    case ColorID::MozCellhighlighttext:
    case ColorID::MozMenuhovertext:
    case ColorID::TargetTextForeground:
      aResult = highlightText;
      return NS_OK;

    case ColorID::Accentcolor:
      aResult = accent;
      return NS_OK;

    case ColorID::Accentcolortext:
      aResult = accentText;
      return NS_OK;

    case ColorID::Scrollbar:
    case ColorID::ThemedScrollbar:
      aResult = window;
      return NS_OK;

    case ColorID::ThemedScrollbarThumb:
      aResult = mid;
      return NS_OK;

    case ColorID::ThemedScrollbarThumbHover:
      aResult = mix(mid, highlight, 0.35f);
      return NS_OK;

    case ColorID::ThemedScrollbarThumbActive:
      aResult = mix(mid, highlight, 0.55f);
      return NS_OK;

    case ColorID::Graytext:
    case ColorID::MozMenuhoverdisabled:
      aResult = disabledText;
      return NS_OK;

    // Infobackground/Infotext handled above (system tip pair).

    case ColorID::Menu:
      aResult = window;
      return NS_OK;

    case ColorID::Menutext:
      aResult = windowText;
      return NS_OK;

    case ColorID::MozOddtreerow:
      aResult = altBase;
      return NS_OK;

    case ColorID::Linktext:
    case ColorID::Activetext:
      aResult = link;
      return NS_OK;

    case ColorID::Visitedtext:
      aResult = linkVisited;
      return NS_OK;

    case ColorID::IMERawInputBackground:
    case ColorID::IMEConvertedTextBackground:
      aResult = NS_TRANSPARENT;
      return NS_OK;

    case ColorID::IMERawInputForeground:
    case ColorID::IMEConvertedTextForeground:
      aResult = NS_SAME_AS_FOREGROUND_COLOR;
      return NS_OK;

    case ColorID::IMERawInputUnderline:
    case ColorID::IMEConvertedTextUnderline:
      aResult = NS_SAME_AS_FOREGROUND_COLOR;
      return NS_OK;

    case ColorID::IMESelectedRawTextUnderline:
    case ColorID::IMESelectedConvertedTextUnderline:
      aResult = NS_TRANSPARENT;
      return NS_OK;

    case ColorID::SpellCheckerUnderline:
      aResult = NS_RGB(0xff, 0, 0);
      return NS_OK;

    default:
      aResult = GetStandinForNativeColor(aID, aScheme);
      return NS_OK;
  }
}

nsresult nsLookAndFeel::NativeGetInt(IntID aID, int32_t& aResult) {
  switch (aID) {
    case IntID::CaretBlinkTime: {
      // Qt reports a full on+off cycle; LookAndFeel wants half-cycle ms.
      const int flash =
          QtAppReady() ? QApplication::cursorFlashTime() : 1060;
      aResult = flash > 0 ? flash / 2 : 530;
      return NS_OK;
    }
    case IntID::CaretBlinkCount:
      // Negative means blink indefinitely.
      aResult = (!QtAppReady() || QApplication::cursorFlashTime() > 0) ? -1 : 0;
      return NS_OK;
    case IntID::CaretWidth:
      aResult = 1;
      return NS_OK;
    case IntID::SelectTextfieldsOnKeyFocus:
      aResult = 1;
      return NS_OK;
    case IntID::SubmenuDelay:
      aResult = StyleHint(QStyle::SH_Menu_SubMenuPopupDelay, 200);
      return NS_OK;
    case IntID::MenusCanOverlapOSBar:
      aResult = 1;
      return NS_OK;
    case IntID::UseOverlayScrollbars:
      // Always overlay on desktop Qt. Classic scrollbars take layout width and
      // appear/disappear when ads/reflows cross the overflow threshold, which
      // shifts the whole page by ~scrollbar width (or 1 CSS px under scale).
      // GTK/Firefox-gtk effectively use overlay on modern desktops; match that.
      aResult = 1;
      return NS_OK;
    case IntID::AllowOverlayScrollbarsOverlap:
      aResult = 1;
      return NS_OK;
    case IntID::SkipNavigatingDisabledMenuItem:
      aResult = 1;
      return NS_OK;
    case IntID::DragThresholdX:
    case IntID::DragThresholdY:
      aResult = QtAppReady() ? QApplication::startDragDistance() : 4;
      if (aResult <= 0) {
        aResult = 4;
      }
      return NS_OK;
    case IntID::UseAccessibilityTheme:
      aResult = 0;
      return NS_OK;
    case IntID::ScrollArrowStyle:
      aResult = eScrollArrowStyle_Single;
      return NS_OK;
    case IntID::ScrollButtonLeftMouseButtonAction:
      aResult = 0;
      return NS_OK;
    case IntID::ScrollButtonMiddleMouseButtonAction:
      aResult = 3;
      return NS_OK;
    case IntID::ScrollButtonRightMouseButtonAction:
      aResult = 3;
      return NS_OK;
    case IntID::TreeOpenDelay:
      aResult = 1000;
      return NS_OK;
    case IntID::TreeCloseDelay:
      aResult = 1000;
      return NS_OK;
    case IntID::TreeLazyScrollDelay:
      aResult = 150;
      return NS_OK;
    case IntID::TreeScrollDelay:
      aResult = 100;
      return NS_OK;
    case IntID::TreeScrollLinesMax:
      aResult = 3;
      return NS_OK;
    case IntID::ChosenMenuItemsShouldBlink:
      aResult = 1;
      return NS_OK;
    case IntID::AlertNotificationOrigin:
      // Bottom-right, vertical slide — common on KDE/GNOME desktops.
      aResult = 0;
      return NS_OK;
    case IntID::ScrollToClick:
      aResult =
          StyleHint(QStyle::SH_ScrollBar_LeftClickAbsolutePosition, 0) ? 1 : 0;
      return NS_OK;
    case IntID::IMERawInputUnderlineStyle:
    case IntID::IMEConvertedTextUnderlineStyle:
      aResult = static_cast<int32_t>(StyleTextDecorationStyle::Solid);
      return NS_OK;
    case IntID::IMESelectedRawTextUnderlineStyle:
    case IntID::IMESelectedConvertedTextUnderline:
      aResult = static_cast<int32_t>(StyleTextDecorationStyle::None);
      return NS_OK;
    case IntID::SpellCheckerUnderlineStyle:
      aResult = static_cast<int32_t>(StyleTextDecorationStyle::Wavy);
      return NS_OK;
    case IntID::MenuBarDrag:
      aResult = 0;
      return NS_OK;
    case IntID::ScrollbarButtonAutoRepeatBehavior:
      aResult = 1;
      return NS_OK;
    case IntID::SwipeAnimationEnabled:
      aResult = 0;
      return NS_OK;
    case IntID::ScrollbarDisplayOnMouseMove:
      aResult = StyleHint(QStyle::SH_ScrollBar_Transient, 0) ? 1 : 0;
      return NS_OK;
    case IntID::ScrollbarFadeBeginDelay:
      aResult = 1000;
      return NS_OK;
    case IntID::ScrollbarFadeDuration:
      aResult = 400;
      return NS_OK;
    case IntID::ContextMenuOffsetVertical:
    case IntID::ContextMenuOffsetHorizontal:
      aResult = 2;
      return NS_OK;
    case IntID::TooltipOffsetVertical:
      aResult = PixelMetric(QStyle::PM_ToolTipLabelFrameWidth, 21);
      if (aResult < 4) {
        aResult = 21;
      }
      return NS_OK;
    case IntID::SystemUsesDarkTheme:
      aResult = SystemIsDark() ? 1 : 0;
      return NS_OK;
    case IntID::PrefersReducedMotion: {
      // No dedicated Qt API; treat disabled style animations as "reduce".
      aResult = StyleHint(QStyle::SH_Widget_Animate, 1) ? 0 : 1;
      return NS_OK;
    }
    case IntID::PrefersReducedTransparency:
      aResult = 0;
      return NS_OK;
    case IntID::InvertedColors:
      aResult = 0;
      return NS_OK;
    case IntID::PrimaryPointerCapabilities:
      // Fine + Hover (mouse-like).
      aResult = 2 | 4;
      return NS_OK;
    case IntID::AllPointerCapabilities: {
      int caps = 2 | 4;  // Fine | Hover
      for (const QInputDevice* device : QInputDevice::devices()) {
        if (device->type() == QInputDevice::DeviceType::TouchScreen) {
          caps |= 1;  // Coarse
        }
      }
      aResult = caps;
      return NS_OK;
    }
    case IntID::SystemScrollbarSize: {
      aResult = PixelMetric(QStyle::PM_ScrollBarExtent, 16);
      if (aResult <= 0) {
        aResult = 16;
      }
      return NS_OK;
    }
    case IntID::TouchDeviceSupportPresent: {
      aResult = 0;
      for (const QInputDevice* device : QInputDevice::devices()) {
        if (device->type() == QInputDevice::DeviceType::TouchScreen) {
          aResult = 1;
          break;
        }
      }
      return NS_OK;
    }
    case IntID::TitlebarRadius:
    case IntID::TooltipRadius:
      aResult = 0;
      return NS_OK;
    case IntID::DynamicRange:
      aResult = 0;
      return NS_OK;
    case IntID::PanelAnimations:
      aResult = StyleHint(QStyle::SH_Widget_Animate, 1) ? 1 : 0;
      return NS_OK;
    case IntID::HideCursorWhileTyping:
      aResult = 0;
      return NS_OK;
    case IntID::FullKeyboardAccess:
      aResult = 1;
      return NS_OK;
    case IntID::PointingDeviceKinds: {
      using Kinds = LookAndFeel::PointingDeviceKinds;
      Kinds kinds = Kinds::Mouse;
      if (QtAppReady()) {
        for (const QInputDevice* device : QInputDevice::devices()) {
          switch (device->type()) {
            case QInputDevice::DeviceType::TouchScreen:
              kinds |= Kinds::Touch;
              break;
            case QInputDevice::DeviceType::Stylus:
              kinds |= Kinds::Pen;
              break;
            default:
              break;
          }
        }
      }
      aResult = static_cast<int32_t>(kinds);
      return NS_OK;
    }
    case IntID::NativeMenubar:
      // Hide in-window menubar when global menu is active. On Wayland require
      // appmenu protocol; on X11 still hide when the feature is enabled so
      // Plasma global menu does not double up with chrome menubar.
      aResult = []() {
#ifdef MOZ_ENABLE_DBUS
        if (!QtAppReady() || !widget::PrefGlobalMenuEnabled()) {
          return false;
        }
        if (QGuiApplication::platformName().contains(QLatin1String("wayland"),
                                                     Qt::CaseInsensitive)) {
          return widget::PrefGlobalMenuWaylandEnabled() &&
                 widget::DBusMenuBar::WaylandAppMenuAvailable();
        }
        return true;
#else
        return false;
#endif
      }();
      return NS_OK;
    case IntID::DialogPrimaryButtonOnLeft: {
      const int layout = StyleHint(QStyle::SH_DialogButtonLayout,
                                   int(QDialogButtonBox::KdeLayout));
      switch (layout) {
        case QDialogButtonBox::MacLayout:
        case QDialogButtonBox::GnomeLayout:
        case QDialogButtonBox::AndroidLayout:
          aResult = 0;
          break;
        case QDialogButtonBox::WinLayout:
        case QDialogButtonBox::KdeLayout:
        default:
          aResult = 1;
          break;
      }
      return NS_OK;
    }
    default:
      // Fall back to generic LookAndFeel defaults rather than hard-failing.
      aResult = 0;
      return NS_OK;
  }
}

nsresult nsLookAndFeel::NativeGetFloat(FloatID aID, float& aResult) {
  switch (aID) {
    case FloatID::IMEUnderlineRelativeSize:
    case FloatID::SpellCheckerUnderlineRelativeSize:
      aResult = 1.0f;
      return NS_OK;
    case FloatID::CaretAspectRatio:
      aResult = -1.0f;
      return NS_OK;
    case FloatID::TextScaleFactor:
      // Qt does not expose a cross-desktop text-scaling factor portably.
      aResult = 1.0f;
      return NS_OK;
    case FloatID::CursorScale:
      aResult = 1.0f;
      return NS_OK;
    default:
      // Prefer a neutral default over hard-failing unknown float IDs.
      aResult = 1.0f;
      return NS_OK;
  }
}

bool nsLookAndFeel::NativeGetFont(FontID aID, nsString& aFontName,
                                  gfxFontStyle& aStyle) {
  if (!QtAppReady()) {
    aFontName.AssignLiteral("sans-serif");
    aStyle.size = 12.0;
    aStyle.style = FontSlantStyle::NORMAL;
    aStyle.weight = FontWeight::NORMAL;
    aStyle.stretch = FontStretch::NORMAL;
    aStyle.systemFont = true;
    return true;
  }
  QFont font = QApplication::font();
  switch (aID) {
    case FontID::MozField:
    case FontID::MozList:
      font = QApplication::font("QLineEdit");
      if (font.family().isEmpty()) {
        font = QApplication::font();
      }
      break;
    case FontID::MozButton:
      font = QApplication::font("QPushButton");
      if (font.family().isEmpty()) {
        font = QApplication::font();
      }
      break;
    case FontID::Menu:
    case FontID::MozPullDownMenu:
      font = QApplication::font("QMenu");
      if (font.family().isEmpty()) {
        font = QApplication::font();
      }
      break;
    case FontID::Caption:
    case FontID::SmallCaption:
      font = QFontDatabase::systemFont(QFontDatabase::TitleFont);
      break;
    case FontID::StatusBar:
      font = QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont);
      break;
    case FontID::MessageBox:
    case FontID::Icon:
    default: {
      // Prefer fixed font only when the style system asks for monospace via
      // other paths; default UI font covers MessageBox/Icon/etc.
      font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
      break;
    }
  }
  if (font.family().isEmpty()) {
    font = QApplication::font();
  }
  FillFontFromQFont(font, aFontName, aStyle);
  return true;
}

char16_t nsLookAndFeel::GetPasswordCharacterImpl() { return char16_t(0x2022); }
