/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "NativeMenuQt.h"
#include "MenuUtilsQt.h"

#include "mozilla/BasicEvents.h"
#include "mozilla/EventDispatcher.h"
#include "QtPrefUtils.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/XULCommandEvent.h"
#include "nsGlobalWindowInner.h"
#include "nsIFrame.h"
#include "nsMenuPopupFrame.h"
#include "nsIWidget.h"
#include "nsPresContext.h"
#include "nsThreadUtils.h"

// From layout/xul/nsMenuPopupFrame.h — avoid pulling layout into widget.
#ifndef POPUPPOSITION_BEFORESTART
#  define POPUPPOSITION_BEFORESTART 0
#  define POPUPPOSITION_BEFOREEND 1
#  define POPUPPOSITION_AFTERSTART 2
#  define POPUPPOSITION_AFTEREND 3
#  define POPUPPOSITION_STARTBEFORE 4
#  define POPUPPOSITION_ENDBEFORE 5
#  define POPUPPOSITION_STARTAFTER 6
#  define POPUPPOSITION_ENDAFTER 7
#  define POPUPPOSITION_OVERLAP 8
#endif
#include "nsWindow.h"
#include <cmath>

#pragma GCC visibility push(default)
#include <QAction>
#include <QCursor>
#include <QMenu>
#include <QPoint>
#include <QScreen>
#include <QGuiApplication>
#include <QWidget>
#pragma GCC visibility pop

namespace mozilla::widget {

static bool PopupIsDisabled(const dom::Element& aElement) {
  return aElement.GetBoolAttr(nsGkAtoms::disabled) ||
         aElement.GetBoolAttr(nsGkAtoms::hidden);
}

static Maybe<bool> PopupGetChecked(const dom::Element& aMenuItem) {
  static dom::Element::AttrValuesArray strings[] = {nsGkAtoms::checkbox,
                                                    nsGkAtoms::radio, nullptr};
  switch (aMenuItem.FindAttrValueIn(kNameSpaceID_None, nsGkAtoms::type, strings,
                                    eCaseMatters)) {
    case 0:
    case 1:
      break;
    default:
      return Nothing();
  }
  return Some(aMenuItem.GetBoolAttr(nsGkAtoms::checked));
}

static MOZ_CAN_RUN_SCRIPT void PopupActivateItem(dom::Element& aElement) {
  if (Maybe<bool> checked = PopupGetChecked(aElement)) {
    if (!aElement.AttrValueIs(kNameSpaceID_None, nsGkAtoms::autocheck,
                              nsGkAtoms::_false, eCaseMatters)) {
      if (!*checked) {
        aElement.SetAttr(kNameSpaceID_None, nsGkAtoms::checked, u"true"_ns,
                         true);
      } else {
        aElement.UnsetAttr(kNameSpaceID_None, nsGkAtoms::checked, true);
      }
    }
  }
  RefPtr doc = aElement.OwnerDoc();
  RefPtr event = new dom::XULCommandEvent(doc, doc->GetPresContext(), nullptr);
  IgnoredErrorResult rv;
  event->InitCommandEvent(u"command"_ns, true, true,
                          nsGlobalWindowInner::Cast(doc->GetInnerWindow()), 0,
                          false, false, false, false, MouseButton::ePrimary,
                          nullptr, 0, rv);
  if (!rv.Failed()) {
    aElement.DispatchEvent(*event);
  }
}

static const dom::Element* PopupMenuPopupChild(const dom::Element& aElement) {
  for (const nsIContent* child = aElement.GetFirstChild(); child;
       child = child->GetNextSibling()) {
    if (child->IsXULElement(nsGkAtoms::menupopup)) {
      return child->AsElement();
    }
  }
  return nullptr;
}

static void PopulateMenu(QMenu* aMenu, const dom::Element& aElement,
                         NativeMenuQt* aOwner) {
  for (const nsIContent* child = aElement.GetFirstChild(); child;
       child = child->GetNextSibling()) {
    if (child->IsXULElement(nsGkAtoms::menuitem) &&
        !PopupIsDisabled(*child->AsElement())) {
      nsAutoString label;
      child->AsElement()->GetAttr(nsGkAtoms::label, label);
      if (label.IsEmpty()) {
        child->AsElement()->GetAttr(nsGkAtoms::aria_label, label);
      }
      QAction* action = aMenu->addAction(
          QString::fromUtf16(label.BeginReading(), int(label.Length())));
      action->setEnabled(
          !child->AsElement()->GetBoolAttr(nsGkAtoms::disabled));
      if (Maybe<bool> checked = PopupGetChecked(*child->AsElement())) {
        action->setCheckable(true);
        action->setChecked(*checked);
      }
      RefPtr element = const_cast<dom::Element*>(child->AsElement());
      ApplyMenuShortcuts(action, element);
      ApplyMenuIcon(action, element);
      // Do not run Gecko command handlers here: that re-enters the event loop
      // during QMenu::activateAction and crashes in hideEvent. Stash the item;
      // FinishHideAndActivate runs it after Qt has closed the menu, but still
      // before popuphidden (so gContextMenu remains valid).
      QObject::connect(action, &QAction::triggered, action,
                       [aOwner, element]() { aOwner->OnItemTriggered(element); });
      continue;
    }
    if (child->IsXULElement(nsGkAtoms::menuseparator)) {
      aMenu->addSeparator();
      continue;
    }
    if (child->IsXULElement(nsGkAtoms::menu) &&
        !PopupIsDisabled(*child->AsElement())) {
      if (const auto* popup = PopupMenuPopupChild(*child->AsElement())) {
        nsAutoString label;
        child->AsElement()->GetAttr(nsGkAtoms::label, label);
        QMenu* sub = aMenu->addMenu(
            QString::fromUtf16(label.BeginReading(), int(label.Length())));
        PopulateMenu(sub, *popup, aOwner);
      }
    }
  }
}

/* static */
bool NativeMenuQt::CanUse() {
  return PrefNativeContextMenus();
}

NativeMenuQt::NativeMenuQt(dom::Element* aPopup)
    : mElement(aPopup), mMenu(mozilla::MakeUnique<QMenu>()) {
  Rebuild();
  // After Qt finishes hiding, run any pending command then fire popuphidden.
  QObject::connect(mMenu.get(), &QMenu::aboutToHide, mMenu.get(), [this]() {
    if (mHideCompletionQueued) {
      return;
    }
    mHideCompletionQueued = true;
    RefPtr<NativeMenuQt> self = this;
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "NativeMenuQt::FinishHideAndActivate",
        [self]() { self->FinishHideAndActivate(); }));
  });
}

NativeMenuQt::~NativeMenuQt() {
  if (mMenu) {
    // Do not call disconnect() with no args — Qt logs
    // "wildcard call disconnects from destroyed signal of QMenu".
    // blockSignals + hide is enough; UniquePtr deletes the QObject cleanly.
    mMenu->blockSignals(true);
    mMenu->hide();
  }
}

void NativeMenuQt::Rebuild() {
  mMenu->clear();
  mPendingActivation = nullptr;
  if (mElement) {
    PopulateMenu(mMenu.get(), *mElement, this);
  }
}

void NativeMenuQt::OnItemTriggered(dom::Element* aItem) {
  if (!aItem) {
    return;
  }
  mPendingActivation = aItem;
  // Match Cocoa: notify before hide / command (closes other menus, sets mode).
  for (NativeMenu::Observer* o : mObservers.Clone()) {
    o->OnNativeMenuWillActivateItem(aItem);
  }
}

void NativeMenuQt::FinishHideAndActivate() {
  mHideCompletionQueued = false;
  // Command before popuphidden: content context menus use gContextMenu which
  // is cleared when the popup hides.
  RefPtr pending = mPendingActivation.forget();
  if (pending) {
    PopupActivateItem(*pending);
  }
  OnAboutToHide();
}

void NativeMenuQt::FirePopupEvent(EventMessage aMessage) {
  if (!mElement) {
    return;
  }
  nsEventStatus status = nsEventStatus_eIgnore;
  WidgetMouseEvent event(true, aMessage, nullptr, WidgetMouseEvent::eReal);
  EventDispatcher::Dispatch(mElement, nullptr, &event, nullptr, &status);
}

static QPoint DeviceToQtGlobal(nsIFrame* aFrame,
                               const LayoutDeviceIntPoint& aDevPos) {
  nsPresContext* pc = aFrame->PresContext();
  nsIWidget* root = pc->GetRootWidget();
  if (root) {
    if (auto* nsWin = nsWindow::FromWidget(root)) {
      const double dpr = nsWin->DevicePixelRatio();
      const auto offset = root->WidgetToScreenOffset();
      return QPoint(int(std::lround((offset.x + aDevPos.x) / dpr)),
                    int(std::lround((offset.y + aDevPos.y) / dpr)));
    }
  }
  return QCursor::pos();
}

// Map XUL popup position to a screen point (device pixels) on aRect.
static LayoutDeviceIntPoint AnchorPointFor(const CSSIntRect& aRect,
                                          int8_t aPosition,
                                          const CSSToLayoutDeviceScale& aScale) {
  CSSIntPoint css;
  switch (aPosition) {
    case POPUPPOSITION_BEFORESTART:
      css = CSSIntPoint(aRect.x, aRect.y);
      break;
    case POPUPPOSITION_BEFOREEND:
      css = CSSIntPoint(aRect.XMost(), aRect.y);
      break;
    case POPUPPOSITION_AFTERSTART:
      css = CSSIntPoint(aRect.x, aRect.YMost());
      break;
    case POPUPPOSITION_AFTEREND:
      css = CSSIntPoint(aRect.XMost(), aRect.YMost());
      break;
    case POPUPPOSITION_STARTBEFORE:
      css = CSSIntPoint(aRect.x, aRect.y);
      break;
    case POPUPPOSITION_ENDBEFORE:
      css = CSSIntPoint(aRect.XMost(), aRect.y);
      break;
    case POPUPPOSITION_STARTAFTER:
      css = CSSIntPoint(aRect.x, aRect.YMost());
      break;
    case POPUPPOSITION_ENDAFTER:
      css = CSSIntPoint(aRect.XMost(), aRect.YMost());
      break;
    case POPUPPOSITION_OVERLAP:
      css = CSSIntPoint(aRect.x, aRect.y);
      break;
    default:
      // after_start-like default (toolbarbutton menus open below).
      css = CSSIntPoint(aRect.x, aRect.YMost());
      break;
  }
  return LayoutDeviceIntPoint::Round(css * aScale);
}

void NativeMenuQt::ShowMenuAnchored(nsIFrame* aClickedFrame,
                                    const nsMenuPopupFrame* aPopupFrame) {
  if (mShowing || !mElement || !aClickedFrame || !aPopupFrame) {
    return;
  }
  Rebuild();
  mShowing = true;
  // Callers already fired popupshowing and approved it.
  FirePopupEvent(eXULPopupShown);
  for (NativeMenu::Observer* o : mObservers.Clone()) {
    o->OnNativeMenuOpened();
  }

  nsPresContext* pc = aClickedFrame->PresContext();
  const CSSIntRect aRect = aPopupFrame->GetScreenAnchorRect();
  const int8_t aPosition = aPopupFrame->GetAlignmentPosition();
  const LayoutDeviceIntPoint devPos =
      AnchorPointFor(aRect, aPosition, pc->CSSToDevPixelScale());
  QPoint global = DeviceToQtGlobal(aClickedFrame, devPos);

  // Keep the menu on-screen when possible.
  if (QScreen* screen = QGuiApplication::screenAt(global)) {
    const QRect geo = screen->availableGeometry();
    const QSize hint = mMenu->sizeHint();
    if (global.x() + hint.width() > geo.right()) {
      global.setX(std::max(geo.left(), geo.right() - hint.width()));
    }
    if (global.y() + hint.height() > geo.bottom()) {
      // Flip above the anchor when it would go off the bottom.
      const LayoutDeviceIntPoint above = LayoutDeviceIntPoint::Round(
          CSSIntPoint(aRect.x, aRect.y) * pc->CSSToDevPixelScale());
      QPoint tryAbove = DeviceToQtGlobal(aClickedFrame, above);
      tryAbove.setY(tryAbove.y() - hint.height());
      if (tryAbove.y() >= geo.top()) {
        global = tryAbove;
      }
    }
  }

  mMenu->popup(global);
}

void NativeMenuQt::ShowMenuAtPosition(nsIFrame* aClickedFrame,
                                      const CSSIntPoint& aPosition,
                                      bool) {
  if (mShowing || !mElement || !aClickedFrame) {
    return;
  }
  Rebuild();
  mShowing = true;
  // Do NOT fire eXULPopupShowing here: ShowNativeMenuInternal already ran
  // FirePopupShowingEvent (which constructs gContextMenu and consumes
  // nsContextMenu.contentData). A second showing re-enters the constructor
  // without contentData → ownerDoc is undefined (console TypeError).
  FirePopupEvent(eXULPopupShown);
  for (NativeMenu::Observer* o : mObservers.Clone()) {
    o->OnNativeMenuOpened();
  }

  nsPresContext* pc = aClickedFrame->PresContext();
  LayoutDeviceIntPoint devPos =
      LayoutDeviceIntPoint::Round(aPosition * pc->CSSToDevPixelScale());
  mMenu->popup(DeviceToQtGlobal(aClickedFrame, devPos));
}

bool NativeMenuQt::Close() {
  if (!mShowing) {
    return false;
  }
  // Synchronous close: no pending activation, fire popup events now.
  mPendingActivation = nullptr;
  const bool wasBlocked = mMenu->blockSignals(true);
  mMenu->hide();
  mMenu->blockSignals(wasBlocked);
  mHideCompletionQueued = false;
  OnAboutToHide();
  return true;
}

void NativeMenuQt::OnAboutToHide() {
  if (!mShowing) {
    return;
  }
  mShowing = false;
  mPendingActivation = nullptr;
  FirePopupEvent(eXULPopupHiding);
  FirePopupEvent(eXULPopupHidden);
  for (NativeMenu::Observer* o : mObservers.Clone()) {
    o->OnNativeMenuClosed();
  }
}

void NativeMenuQt::ActivateItem(dom::Element*, Modifiers, int16_t,
                                ErrorResult&) {}

void NativeMenuQt::OpenSubmenu(dom::Element*) {}

void NativeMenuQt::CloseSubmenu(dom::Element*) {}

RefPtr<dom::Element> NativeMenuQt::Element() { return mElement; }

}  // namespace mozilla::widget
