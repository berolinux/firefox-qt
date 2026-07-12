/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Platform.h"
#include "QtAccessible.h"
#include "AccessibleWrap.h"

#include "nsIAccessibleEvent.h"
#include "nsAccessibilityService.h"
#include "States.h"
#include "mozilla/a11y/RemoteAccessible.h"
#include "prenv.h"

#pragma GCC visibility push(default)
#include <QAccessible>
#include <QAccessibleEvent>
#include <QGuiApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QVariant>
#pragma GCC visibility pop

using namespace mozilla;
using namespace mozilla::a11y;

static bool sA11yEnabled = false;
static bool sA11yCacheValid = false;

static QAccessibleInterface* WrapperFor(Accessible* aAcc) {
  return QtAccessible::InterfaceFor(aAcc);
}

void a11y::PlatformInit() {
  InitQtAccessibility();
  sA11yCacheValid = false;
}

void a11y::PlatformShutdown() {
  ShutdownQtAccessibility();
  sA11yCacheValid = false;
}

void a11y::ProxyCreated(RemoteAccessible* aProxy) {
  MOZ_ASSERT(aProxy);
  if (aProxy->GetWrapper()) {
    return;
  }
  auto* iface = new QtAccessible(aProxy, nullptr);
  QAccessible::registerAccessibleInterface(iface);
  aProxy->SetWrapper(reinterpret_cast<uintptr_t>(iface));
}

void a11y::ProxyDestroyed(RemoteAccessible* aProxy) {
  auto wrap = aProxy->GetWrapper();
  if (!wrap) {
    return;
  }
  auto* iface = reinterpret_cast<QtAccessible*>(wrap);
  iface->ClearAcc();
  const QAccessible::Id id = QAccessible::uniqueId(iface);
  if (id) {
    QAccessible::deleteAccessibleInterface(id);
  }
  aProxy->SetWrapper(0);
}

static void FireQtEvent(Accessible* aTarget, QAccessible::Event aEvent) {
  QAccessibleInterface* iface = WrapperFor(aTarget);
  if (!iface) {
    return;
  }
  QAccessibleEvent event(iface, aEvent);
  QAccessible::updateAccessibility(&event);
}

void a11y::PlatformEvent(Accessible* aTarget, uint32_t aEventType) {
  if (!aTarget) {
    return;
  }
  switch (aEventType) {
    case nsIAccessibleEvent::EVENT_NAME_CHANGE:
      FireQtEvent(aTarget, QAccessible::NameChanged);
      break;
    case nsIAccessibleEvent::EVENT_DESCRIPTION_CHANGE:
      FireQtEvent(aTarget, QAccessible::DescriptionChanged);
      break;
    case nsIAccessibleEvent::EVENT_VALUE_CHANGE:
    case nsIAccessibleEvent::EVENT_TEXT_VALUE_CHANGE: {
      QAccessibleInterface* iface = WrapperFor(aTarget);
      if (!iface) {
        break;
      }
      nsAutoString value;
      aTarget->Value(value);
      QAccessibleValueChangeEvent event(
          iface, QString::fromUtf16(
                     reinterpret_cast<const char16_t*>(value.BeginReading()),
                     int(value.Length())));
      QAccessible::updateAccessibility(&event);
      break;
    }
    case nsIAccessibleEvent::EVENT_DOCUMENT_LOAD_COMPLETE:
      FireQtEvent(aTarget, QAccessible::DocumentLoadComplete);
      break;
    case nsIAccessibleEvent::EVENT_DOCUMENT_RELOAD:
      FireQtEvent(aTarget, QAccessible::DocumentReload);
      break;
    case nsIAccessibleEvent::EVENT_DOCUMENT_LOAD_STOPPED:
      FireQtEvent(aTarget, QAccessible::DocumentLoadStopped);
      break;
    case nsIAccessibleEvent::EVENT_MENUPOPUP_START:
      FireQtEvent(aTarget, QAccessible::PopupMenuStart);
      break;
    case nsIAccessibleEvent::EVENT_MENUPOPUP_END:
      FireQtEvent(aTarget, QAccessible::PopupMenuEnd);
      break;
    case nsIAccessibleEvent::EVENT_ALERT:
      FireQtEvent(aTarget, QAccessible::Alert);
      break;
    case nsIAccessibleEvent::EVENT_REORDER:
      FireQtEvent(aTarget, QAccessible::ObjectReorder);
      break;
    case nsIAccessibleEvent::EVENT_SELECTION:
      FireQtEvent(aTarget, QAccessible::Selection);
      break;
    case nsIAccessibleEvent::EVENT_SELECTION_ADD:
      FireQtEvent(aTarget, QAccessible::SelectionAdd);
      break;
    case nsIAccessibleEvent::EVENT_SELECTION_REMOVE:
      FireQtEvent(aTarget, QAccessible::SelectionRemove);
      break;
    case nsIAccessibleEvent::EVENT_SELECTION_WITHIN:
      FireQtEvent(aTarget, QAccessible::SelectionWithin);
      break;
    case nsIAccessibleEvent::EVENT_SCROLLING_START:
      FireQtEvent(aTarget, QAccessible::ScrollingStart);
      break;
    case nsIAccessibleEvent::EVENT_SCROLLING_END:
      FireQtEvent(aTarget, QAccessible::ScrollingEnd);
      break;
    default:
      break;
  }
}

void a11y::PlatformStateChangeEvent(Accessible* aTarget, uint64_t aState,
                                    bool aEnabled) {
  QAccessibleInterface* iface = WrapperFor(aTarget);
  if (!iface) {
    return;
  }
  QAccessible::State stateChange;
  // Map the changed Gecko state bit onto the Qt state field.
  if (aState & states::CHECKED) {
    stateChange.checked = true;
  }
  if (aState & states::SELECTED) {
    stateChange.selected = true;
  }
  if (aState & states::FOCUSED) {
    stateChange.focused = true;
  }
  if (aState & states::EXPANDED) {
    stateChange.expanded = true;
  }
  if (aState & states::COLLAPSED) {
    stateChange.collapsed = true;
  }
  if (aState & states::BUSY) {
    stateChange.busy = true;
  }
  if (aState & states::UNAVAILABLE) {
    stateChange.disabled = true;
  }
  if (aState & states::INVISIBLE) {
    stateChange.invisible = true;
  }
  if (aState & states::PRESSED) {
    stateChange.pressed = true;
  }
  QAccessibleStateChangeEvent event(iface, stateChange);
  // Qt uses the bitfield as a mask of changed states.
  (void)aEnabled;
  QAccessible::updateAccessibility(&event);
}

void a11y::PlatformFocusEvent(Accessible* aTarget) {
  FireQtEvent(aTarget, QAccessible::Focus);
}

void a11y::PlatformCaretMoveEvent(Accessible* aTarget, int32_t aOffset,
                                  bool aIsSelectionCollapsed,
                                  int32_t aGranularity, bool aFromUser) {
  QAccessibleInterface* iface = WrapperFor(aTarget);
  if (!iface) {
    return;
  }
  QAccessibleTextCursorEvent event(iface, aOffset);
  QAccessible::updateAccessibility(&event);
  if (!aIsSelectionCollapsed) {
    QAccessibleTextSelectionEvent sel(iface, aOffset, aOffset);
    QAccessible::updateAccessibility(&sel);
  }
  (void)aGranularity;
  (void)aFromUser;
}

void a11y::PlatformTextChangeEvent(Accessible* aTarget, const nsAString& aStr,
                                   int32_t aStart, uint32_t aLen, bool aIsInsert,
                                   bool aFromUser) {
  QAccessibleInterface* iface = WrapperFor(aTarget);
  if (!iface) {
    return;
  }
  const QString text = QString::fromUtf16(
      reinterpret_cast<const char16_t*>(aStr.BeginReading()), int(aStr.Length()));
  if (aIsInsert) {
    QAccessibleTextInsertEvent event(iface, aStart, text);
    QAccessible::updateAccessibility(&event);
  } else {
    QAccessibleTextRemoveEvent event(iface, aStart, text);
    QAccessible::updateAccessibility(&event);
  }
  (void)aLen;
  (void)aFromUser;
}

void a11y::PlatformShowHideEvent(Accessible* aTarget, Accessible* aParent,
                                 bool aInsert, bool aFromUser) {
  FireQtEvent(aTarget, aInsert ? QAccessible::ObjectShow : QAccessible::ObjectHide);
  (void)aParent;
  (void)aFromUser;
}

void a11y::PlatformSelectionEvent(Accessible* aTarget, Accessible* aWidget,
                                  uint32_t aType) {
  PlatformEvent(aTarget, aType);
  (void)aWidget;
}

void a11y::PlatformAnnouncementEvent(Accessible* aTarget,
                                     const nsAString& aAnnouncement,
                                     uint16_t aPriority) {
  QAccessibleInterface* iface = WrapperFor(aTarget);
  if (!iface) {
    return;
  }
  const QString msg = QString::fromUtf16(
      reinterpret_cast<const char16_t*>(aAnnouncement.BeginReading()),
      int(aAnnouncement.Length()));
  QAccessibleAnnouncementEvent event(iface, msg);
  if (aPriority) {
    event.setPoliteness(QAccessible::AnnouncementPoliteness::Assertive);
  }
  QAccessible::updateAccessibility(&event);
}

bool a11y::ShouldA11yBeEnabled() {
  EPlatformDisabledState disabledState = PlatformDisabledState();
  if (disabledState == ePlatformIsDisabled) {
    return false;
  }
  if (disabledState == ePlatformIsForceEnabled) {
    return true;
  }

  if (const char* envValue = PR_GetEnv("GNOME_ACCESSIBILITY")) {
    return !!atoi(envValue);
  }
  if (const char* envValue = PR_GetEnv("QT_ACCESSIBILITY")) {
    return !!atoi(envValue);
  }

  if (sA11yCacheValid) {
    return sA11yEnabled;
  }

  // Qt reports active once an AT client is connected via the AT-SPI bridge.
  if (QAccessible::isActive()) {
    sA11yEnabled = true;
    sA11yCacheValid = true;
    return true;
  }

  // Check FreeDesktop org.a11y.Status.IsEnabled (same bus GTK uses).
  QDBusInterface iface(QStringLiteral("org.a11y.Bus"),
                       QStringLiteral("/org/a11y/bus"),
                       QStringLiteral("org.a11y.Status"),
                       QDBusConnection::sessionBus());
  if (iface.isValid()) {
    QVariant v = iface.property("IsEnabled");
    if (v.isValid()) {
      sA11yEnabled = v.toBool();
      sA11yCacheValid = true;
      return sA11yEnabled;
    }
  }

  sA11yEnabled = false;
  sA11yCacheValid = true;
  return false;
}

uint64_t a11y::GetCacheDomainsForKnownClients(uint64_t aCacheDomains) {
  return aCacheDomains;
}

void a11y::GetHumanReadableInstantiatorStr(nsAString& aResult) {
  aResult.Truncate();
  if (QAccessible::isActive()) {
    aResult.AssignLiteral("QtAccessible");
  }
}
