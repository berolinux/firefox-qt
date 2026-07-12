/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "QtAccessible.h"
#include "AccessibleWrap.h"

#include "DocAccessible.h"
#include "LocalAccessible-inl.h"
#include "AccAttributes.h"
#include "nsAccUtils.h"
#include "nsAccessibilityService.h"
#include "nsCoreUtils.h"
#include "nsIWidget.h"
#include "nsWindow.h"
#include "QtWindowBridge.h"
#include "RootAccessible.h"
#include "States.h"
#include "mozilla/PresShell.h"
#include "mozilla/a11y/RemoteAccessible.h"
#include "mozilla/a11y/Role.h"
#include "mozilla/a11y/HyperTextAccessibleBase.h"
#include "mozilla/a11y/TableAccessible.h"
#include "mozilla/a11y/TableCellAccessible.h"
#include "nsIAccessibleTypes.h"
#include "nsString.h"

#pragma GCC visibility push(default)
#include <QGuiApplication>
#include <QAccessible>
#include <cmath>
#pragma GCC visibility pop

using namespace mozilla;
using namespace mozilla::a11y;

static QString ToQString(const nsAString& aStr) {
  return QString::fromUtf16(reinterpret_cast<const char16_t*>(aStr.BeginReading()),
                            int(aStr.Length()));
}

static nsString ToNsString(const QString& aStr) {
  nsString out;
  out.Assign(reinterpret_cast<const char16_t*>(aStr.utf16()), aStr.size());
  return out;
}

// ---------------------------------------------------------------------------
// Role / state mapping
// ---------------------------------------------------------------------------

QAccessible::Role QtAccessible::RoleFor(mozilla::a11y::role aRole) {
  switch (aRole) {
    case roles::MENUBAR:
      return QAccessible::MenuBar;
    case roles::SCROLLBAR:
      return QAccessible::ScrollBar;
    case roles::ALERT:
      return QAccessible::AlertMessage;
    case roles::MENUPOPUP:
      return QAccessible::PopupMenu;
    case roles::MENUITEM:
    case roles::CHECK_MENU_ITEM:
    case roles::RADIO_MENU_ITEM:
    case roles::PARENT_MENUITEM:
      return QAccessible::MenuItem;
    case roles::TOOLTIP:
      return QAccessible::ToolTip;
    case roles::APPLICATION:
      return QAccessible::Application;
    case roles::DOCUMENT:
      return QAccessible::WebDocument;
    case roles::PANE:
    case roles::INTERNAL_FRAME:
      return QAccessible::Pane;
    case roles::DIALOG:
      return QAccessible::Dialog;
    case roles::GROUPING:
    case roles::FORM:
      return QAccessible::Grouping;
    case roles::SECTION:
      return QAccessible::Section;
    case roles::SEPARATOR:
      return QAccessible::Separator;
    case roles::TOOLBAR:
      return QAccessible::ToolBar;
    case roles::STATUSBAR:
      return QAccessible::StatusBar;
    case roles::TABLE:
      return QAccessible::Table;
    case roles::COLUMNHEADER:
      return QAccessible::ColumnHeader;
    case roles::ROWHEADER:
      return QAccessible::RowHeader;
    case roles::ROW:
      return QAccessible::Row;
    case roles::CELL:
    case roles::GRID_CELL:
    case roles::MATHML_CELL:
      return QAccessible::Cell;
    case roles::LINK:
      return QAccessible::Link;
    case roles::LIST:
    case roles::LISTBOX:
      return QAccessible::List;
    case roles::LISTITEM:
    case roles::OPTION:
      return QAccessible::ListItem;
    case roles::OUTLINE:
    case roles::TREE_TABLE:
      return QAccessible::Tree;
    case roles::OUTLINEITEM:
      return QAccessible::TreeItem;
    case roles::PAGETAB:
      return QAccessible::PageTab;
    case roles::PAGETABLIST:
      return QAccessible::PageTabList;
    case roles::PROPERTYPAGE:
      return QAccessible::PropertyPage;
    case roles::GRAPHIC:
    case roles::IMAGE_MAP:
      return QAccessible::Graphic;
    case roles::STATICTEXT:
    case roles::LABEL:
    case roles::TEXT_LEAF:
      return QAccessible::StaticText;
    case roles::WHITESPACE:
      return QAccessible::Whitespace;
    case roles::ENTRY:
    case roles::PASSWORD_TEXT:
    case roles::EDITCOMBOBOX:
      return QAccessible::EditableText;
    case roles::PUSHBUTTON:
    case roles::TOGGLE_BUTTON:
      return QAccessible::Button;
    case roles::BUTTONDROPDOWN:
      return QAccessible::ButtonDropDown;
    case roles::BUTTONMENU:
      return QAccessible::ButtonMenu;
    case roles::CHECKBUTTON:
      return QAccessible::CheckBox;
    case roles::SWITCH:
      return QAccessible::Switch;
    case roles::RADIOBUTTON:
      return QAccessible::RadioButton;
    case roles::COMBOBOX:
      return QAccessible::ComboBox;
    case roles::PROGRESSBAR:
    case roles::METER:
      return QAccessible::ProgressBar;
    case roles::SLIDER:
      return QAccessible::Slider;
    case roles::SPINBUTTON:
      return QAccessible::SpinBox;
    case roles::DIAGRAM:
    case roles::CANVAS:
      return QAccessible::Canvas;
    case roles::ANIMATION:
      return QAccessible::Animation;
    case roles::FLAT_EQUATION:
    case roles::MATHML_MATH:
      return QAccessible::Equation;
    case roles::PARAGRAPH:
      return QAccessible::Paragraph;
    case roles::HEADING:
      return QAccessible::Heading;
    case roles::NOTE:
      return QAccessible::Notification;
    case roles::CHROME_WINDOW:
    case roles::APP_ROOT:
      return QAccessible::Window;
    case roles::NOTHING:
    default:
      return QAccessible::Client;
  }
}

QAccessible::State QtAccessible::StateFor(uint64_t s) {
  QAccessible::State st;
  st.disabled = !!(s & states::UNAVAILABLE);
  st.selected = !!(s & states::SELECTED);
  st.focusable = !!(s & states::FOCUSABLE);
  st.focused = !!(s & states::FOCUSED);
  st.pressed = !!(s & states::PRESSED);
  st.checkable = !!(s & states::CHECKABLE);
  st.checked = !!(s & states::CHECKED);
  st.checkStateMixed = !!(s & states::MIXED);
  st.readOnly = !!(s & states::READONLY);
  st.hotTracked = !!(s & states::HOTTRACKED);
  st.defaultButton = !!(s & states::DEFAULT);
  st.expanded = !!(s & states::EXPANDED);
  st.collapsed = !!(s & states::COLLAPSED);
  st.busy = !!(s & states::BUSY);
  st.expandable = !!(s & states::EXPANDABLE);
  st.invisible = !!(s & states::INVISIBLE);
  st.offscreen = !!(s & states::OFFSCREEN);
  st.selectable = !!(s & states::SELECTABLE);
  st.multiSelectable = !!(s & states::MULTISELECTABLE);
  st.extSelectable = !!(s & states::EXTSELECTABLE);
  st.passwordEdit = !!(s & states::PROTECTED);
  st.hasPopup = !!(s & states::HASPOPUP);
  st.modal = !!(s & states::MODAL);
  st.editable = !!(s & states::EDITABLE);
  st.multiLine = !!(s & states::MULTI_LINE);
  st.selectableText = !!(s & states::SELECTABLE_TEXT);
  st.invalid = !!(s & states::DEFUNCT);
  st.linked = !!(s & states::LINKED);
  st.traversed = !!(s & states::TRAVERSED);
  st.sizeable = !!(s & states::SIZEABLE);
  return st;
}

// ---------------------------------------------------------------------------
// Window lookup
// ---------------------------------------------------------------------------

QWindow* QtAccessible::WindowFor(Accessible* aAcc) {
  if (!aAcc) {
    return nullptr;
  }
  LocalAccessible* local = aAcc->AsLocal();
  if (!local) {
    Accessible* p = aAcc;
    while (p && p->IsRemote()) {
      p = p->Parent();
    }
    local = p ? p->AsLocal() : nullptr;
  }
  if (!local) {
    return nullptr;
  }
  DocAccessible* doc = local->Document();
  if (!doc || !doc->PresShellPtr()) {
    return nullptr;
  }
  nsIWidget* widget = doc->PresShellPtr()->GetRootWidget();
  if (!widget) {
    return nullptr;
  }
  auto* win = static_cast<nsWindow*>(widget);
  if (!win) {
    return nullptr;
  }
  return static_cast<QWindow*>(win->GetQtWindow());
}

// ---------------------------------------------------------------------------
// Interface lookup
// ---------------------------------------------------------------------------

QAccessibleInterface* QtAccessible::InterfaceFor(Accessible* aAcc) {
  if (!aAcc) {
    return nullptr;
  }
  if (LocalAccessible* local = aAcc->AsLocal()) {
    if (local->IsDefunct()) {
      return nullptr;
    }
  }
  if (aAcc->IsRemote()) {
    auto* remote = aAcc->AsRemote();
    if (uintptr_t wrap = remote->GetWrapper()) {
      return reinterpret_cast<QtAccessible*>(wrap);
    }
    return nullptr;
  }
  void* p = nullptr;
  aAcc->AsLocal()->GetNativeInterface(&p);
  return static_cast<QAccessibleInterface*>(p);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

QtAccessible::QtAccessible(Accessible* aAcc, QObject* aObject)
    : mAcc(aAcc), mObject(aObject) {}

bool QtAccessible::isValid() const {
  if (!mAcc) {
    return false;
  }
  if (LocalAccessible* local = mAcc->AsLocal()) {
    return !local->IsDefunct();
  }
  return !(mAcc->State() & states::DEFUNCT);
}

QObject* QtAccessible::object() const { return mObject; }

QWindow* QtAccessible::window() const {
  if (auto* w = qobject_cast<QWindow*>(mObject)) {
    return w;
  }
  return WindowFor(mAcc);
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

QAccessibleInterface* QtAccessible::parent() const {
  if (!isValid()) {
    return nullptr;
  }
  Accessible* p = mAcc->Parent();
  if (!p) {
    // Root's parent is the QWindow shell (via object association) or null.
    if (mObject) {
      return nullptr;
    }
    return nullptr;
  }
  return InterfaceFor(p);
}

int QtAccessible::childCount() const {
  if (!isValid()) {
    return 0;
  }
  return int(mAcc->ChildCount());
}

QAccessibleInterface* QtAccessible::child(int index) const {
  if (!isValid() || index < 0 || uint32_t(index) >= mAcc->ChildCount()) {
    return nullptr;
  }
  return InterfaceFor(mAcc->ChildAt(uint32_t(index)));
}

int QtAccessible::indexOfChild(const QAccessibleInterface* child) const {
  if (!isValid() || !child) {
    return -1;
  }
  auto* qtChild = static_cast<const QtAccessible*>(child);
  if (!qtChild->mAcc) {
    return -1;
  }
  return qtChild->mAcc->IndexInParent();
}

QAccessibleInterface* QtAccessible::childAt(int x, int y) const {
  if (!isValid()) {
    return nullptr;
  }
  Accessible* found =
      mAcc->ChildAtPoint(x, y, Accessible::EWhichChildAtPoint::DeepestChild);
  if (!found || found == mAcc) {
    // Direct child fallback.
    found = mAcc->ChildAtPoint(x, y, Accessible::EWhichChildAtPoint::DirectChild);
  }
  if (!found || found == mAcc) {
    return nullptr;
  }
  return InterfaceFor(found);
}

QAccessibleInterface* QtAccessible::focusChild() const {
  if (!isValid()) {
    return nullptr;
  }
  // Walk focused descendant if any.
  if (mAcc->State() & states::FOCUSED) {
    return const_cast<QtAccessible*>(this);
  }
  const uint32_t n = mAcc->ChildCount();
  for (uint32_t i = 0; i < n; ++i) {
    if (QAccessibleInterface* c = child(int(i))) {
      if (QAccessibleInterface* f = c->focusChild()) {
        return f;
      }
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

QString QtAccessible::text(QAccessible::Text t) const {
  if (!isValid()) {
    return {};
  }
  nsAutoString str;
  switch (t) {
    case QAccessible::Name:
      mAcc->Name(str);
      break;
    case QAccessible::Description:
      mAcc->Description(str);
      break;
    case QAccessible::Value:
      mAcc->Value(str);
      break;
    case QAccessible::Help:
      break;
    case QAccessible::Accelerator: {
      // Keyboard shortcut if any — Accessible has AccessKey / KeyboardShortcut.
      mAcc->AccessKey().ToString(str);
      break;
    }
    default:
      break;
  }
  return ToQString(str);
}

void QtAccessible::setText(QAccessible::Text t, const QString& value) {
  if (!isValid() || t != QAccessible::Value) {
    return;
  }
  if (HyperTextAccessibleBase* ht = AsHyperText()) {
    if (mAcc->State() & states::EDITABLE) {
      ht->ReplaceText(ToNsString(value));
    }
  }
}

QRect QtAccessible::rect() const {
  if (!isValid()) {
    return {};
  }
  LayoutDeviceIntRect r = mAcc->Bounds();
  return QRect(r.X(), r.Y(), r.Width(), r.Height());
}

QAccessible::Role QtAccessible::role() const {
  if (!isValid()) {
    return QAccessible::NoRole;
  }
  return RoleFor(mAcc->Role());
}

QAccessible::State QtAccessible::state() const {
  if (!isValid()) {
    QAccessible::State st;
    st.invalid = true;
    return st;
  }
  return StateFor(mAcc->State());
}

// ---------------------------------------------------------------------------
// Optional interfaces
// ---------------------------------------------------------------------------

HyperTextAccessibleBase* QtAccessible::AsHyperText() const {
  return mAcc ? mAcc->AsHyperTextBase() : nullptr;
}

TableAccessible* QtAccessible::AsTable() const {
  return mAcc ? mAcc->AsTable() : nullptr;
}

TableCellAccessible* QtAccessible::AsTableCell() const {
  return mAcc ? mAcc->AsTableCell() : nullptr;
}

bool QtAccessible::HasTextInterface() const {
  return AsHyperText() != nullptr;
}

bool QtAccessible::HasValueInterface() const {
  if (!mAcc) {
    return false;
  }
  // Numeric value if min != max or role is value-like.
  const double minV = mAcc->MinValue();
  const double maxV = mAcc->MaxValue();
  if (!std::isnan(minV) && !std::isnan(maxV) && minV != maxV) {
    return true;
  }
  switch (mAcc->Role()) {
    case roles::SLIDER:
    case roles::SPINBUTTON:
    case roles::PROGRESSBAR:
    case roles::METER:
    case roles::SCROLLBAR:
      return true;
    default:
      return false;
  }
}

bool QtAccessible::HasActionInterface() const {
  return mAcc &&
         (mAcc->ActionCount() > 0 || (mAcc->State() & states::FOCUSABLE));
}

bool QtAccessible::HasTableInterface() const { return AsTable() != nullptr; }

bool QtAccessible::HasTableCellInterface() const {
  return AsTableCell() != nullptr;
}

void* QtAccessible::interface_cast(QAccessible::InterfaceType type) {
  switch (type) {
    case QAccessible::ActionInterface:
      if (HasActionInterface()) {
        return static_cast<QAccessibleActionInterface*>(this);
      }
      break;
    case QAccessible::TextInterface:
      if (HasTextInterface()) {
        return static_cast<QAccessibleTextInterface*>(this);
      }
      break;
    case QAccessible::EditableTextInterface:
      if (HasTextInterface() && mAcc && (mAcc->State() & states::EDITABLE)) {
        return static_cast<QAccessibleEditableTextInterface*>(this);
      }
      break;
    case QAccessible::ValueInterface:
      if (HasValueInterface()) {
        return static_cast<QAccessibleValueInterface*>(this);
      }
      break;
    case QAccessible::TableInterface:
      if (HasTableInterface()) {
        return static_cast<QAccessibleTableInterface*>(this);
      }
      break;
    case QAccessible::TableCellInterface:
      if (HasTableCellInterface()) {
        return static_cast<QAccessibleTableCellInterface*>(this);
      }
      break;
    default:
      break;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Action
// ---------------------------------------------------------------------------

QStringList QtAccessible::actionNames() const {
  QStringList names;
  if (!isValid()) {
    return names;
  }
  const uint8_t count = mAcc->ActionCount();
  for (uint8_t i = 0; i < count; ++i) {
    nsAutoString name;
    mAcc->ActionNameAt(i, name);
    if (name.IsEmpty()) {
      names << QStringLiteral("default");
    } else {
      names << ToQString(name);
    }
  }
  if (names.isEmpty() && (mAcc->State() & states::FOCUSABLE)) {
    names << QAccessibleActionInterface::setFocusAction();
  }
  return names;
}

QString QtAccessible::localizedActionDescription(const QString& name) const {
  return name;
}

void QtAccessible::doAction(const QString& actionName) {
  if (!isValid()) {
    return;
  }
  if (actionName == QAccessibleActionInterface::setFocusAction()) {
    mAcc->TakeFocus();
    return;
  }
  const uint8_t count = mAcc->ActionCount();
  for (uint8_t i = 0; i < count; ++i) {
    nsAutoString name;
    mAcc->ActionNameAt(i, name);
    if (ToQString(name) == actionName ||
        (name.IsEmpty() && actionName == QLatin1String("default"))) {
      mAcc->DoAction(i);
      return;
    }
  }
  if (count > 0) {
    mAcc->DoAction(0);
  }
}

QStringList QtAccessible::keyBindingsForAction(const QString&) const {
  return {};
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void QtAccessible::selection(int selectionIndex, int* startOffset,
                             int* endOffset) const {
  if (startOffset) {
    *startOffset = 0;
  }
  if (endOffset) {
    *endOffset = 0;
  }
  HyperTextAccessibleBase* ht = AsHyperText();
  if (!ht || selectionIndex != 0) {
    return;
  }
  int32_t start = 0, end = 0;
  if (ht->SelectionBoundsAt(0, &start, &end)) {
    if (startOffset) {
      *startOffset = start;
    }
    if (endOffset) {
      *endOffset = end;
    }
  }
}

int QtAccessible::selectionCount() const {
  HyperTextAccessibleBase* ht = AsHyperText();
  if (!ht) {
    return 0;
  }
  int32_t start = 0, end = 0;
  return ht->SelectionBoundsAt(0, &start, &end) && start != end ? 1 : 0;
}

void QtAccessible::addSelection(int startOffset, int endOffset) {
  if (HyperTextAccessibleBase* ht = AsHyperText()) {
    ht->SetSelectionBoundsAt(-1, startOffset, endOffset);
  }
}

void QtAccessible::removeSelection(int selectionIndex) {
  if (selectionIndex != 0) {
    return;
  }
  if (HyperTextAccessibleBase* ht = AsHyperText()) {
    const int32_t caret = ht->CaretOffset();
    ht->SetSelectionBoundsAt(0, caret, caret);
  }
}

void QtAccessible::setSelection(int selectionIndex, int startOffset,
                                int endOffset) {
  if (selectionIndex != 0) {
    return;
  }
  if (HyperTextAccessibleBase* ht = AsHyperText()) {
    ht->SetSelectionBoundsAt(0, startOffset, endOffset);
  }
}

int QtAccessible::cursorPosition() const {
  HyperTextAccessibleBase* ht = AsHyperText();
  return ht ? ht->CaretOffset() : 0;
}

void QtAccessible::setCursorPosition(int position) {
  if (HyperTextAccessibleBase* ht = AsHyperText()) {
    ht->SetCaretOffset(position);
  }
}

QString QtAccessible::text(int startOffset, int endOffset) const {
  HyperTextAccessibleBase* ht = AsHyperText();
  if (!ht) {
    return {};
  }
  nsAutoString str;
  ht->TextSubstring(startOffset, endOffset, str);
  return ToQString(str);
}

int QtAccessible::characterCount() const {
  HyperTextAccessibleBase* ht = AsHyperText();
  return ht ? int(ht->CharacterCount()) : 0;
}

QRect QtAccessible::characterRect(int offset) const {
  HyperTextAccessibleBase* ht = AsHyperText();
  if (!ht) {
    return rect();
  }
  LayoutDeviceIntRect r =
      ht->CharBounds(offset, nsIAccessibleCoordinateType::COORDTYPE_SCREEN_RELATIVE);
  return QRect(r.x, r.y, r.width, r.height);
}

int QtAccessible::offsetAtPoint(const QPoint& point) const {
  HyperTextAccessibleBase* ht = AsHyperText();
  if (!ht) {
    return -1;
  }
  return ht->OffsetAtPoint(point.x(), point.y(),
                           nsIAccessibleCoordinateType::COORDTYPE_SCREEN_RELATIVE);
}

void QtAccessible::scrollToSubstring(int startIndex, int endIndex) {
  if (HyperTextAccessibleBase* ht = AsHyperText()) {
    ht->ScrollSubstringTo(startIndex, endIndex,
                          nsIAccessibleScrollType::SCROLL_TYPE_ANYWHERE);
  }
}

QString QtAccessible::attributes(int offset, int* startOffset,
                                  int* endOffset) const {
  // QAccessible expects "key:value;key:value;" (same shape as IA2).
  if (startOffset) {
    *startOffset = 0;
  }
  if (endOffset) {
    *endOffset = 0;
  }
  HyperTextAccessibleBase* ht = AsHyperText();
  if (!ht) {
    if (endOffset) {
      *endOffset = characterCount();
    }
    return {};
  }
  int32_t start = 0;
  int32_t end = 0;
  RefPtr<AccAttributes> attrs =
      ht->TextAttributes(/* aIncludeDefAttrs */ true, offset, &start, &end);
  if (startOffset) {
    *startOffset = start;
  }
  if (endOffset) {
    *endOffset = end;
  }
  if (!attrs) {
    return {};
  }
  QString out;
  for (auto iter : *attrs) {
    nsAutoString name;
    nsAutoString value;
    iter.NameAsString(name);
    iter.ValueAsString(value);
    // Escape ':', ';', '=', ',', '\\' for attribute parsers.
    auto escape = [](const nsAString& in) {
      nsString s(in);
      for (uint32_t i = 0; i < s.Length();) {
        const char16_t c = s[i];
        if (c == u':' || c == u';' || c == u'=' || c == u',' || c == u'\\') {
          s.Insert(u'\\', i);
          i += 2;
        } else {
          ++i;
        }
      }
      return ToQString(s);
    };
    out += escape(name);
    out += QLatin1Char(':');
    out += escape(value);
    out += QLatin1Char(';');
  }
  return out;
}

// ---------------------------------------------------------------------------
// Editable text
// ---------------------------------------------------------------------------

void QtAccessible::deleteText(int startOffset, int endOffset) {
  if (HyperTextAccessibleBase* ht = AsHyperText()) {
    ht->DeleteText(startOffset, endOffset);
  }
}

void QtAccessible::insertText(int offset, const QString& value) {
  if (HyperTextAccessibleBase* ht = AsHyperText()) {
    ht->InsertText(ToNsString(value), offset);
  }
}

void QtAccessible::replaceText(int startOffset, int endOffset,
                               const QString& value) {
  if (HyperTextAccessibleBase* ht = AsHyperText()) {
    ht->DeleteText(startOffset, endOffset);
    ht->InsertText(ToNsString(value), startOffset);
  }
}

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

QVariant QtAccessible::currentValue() const {
  if (!isValid()) {
    return {};
  }
  const double v = mAcc->CurValue();
  return std::isnan(v) ? QVariant{} : QVariant(v);
}

void QtAccessible::setCurrentValue(const QVariant& value) {
  if (!isValid()) {
    return;
  }
  bool ok = false;
  const double v = value.toDouble(&ok);
  if (ok) {
    mAcc->SetCurValue(v);
  }
}

QVariant QtAccessible::maximumValue() const {
  if (!isValid()) {
    return {};
  }
  const double v = mAcc->MaxValue();
  return std::isnan(v) ? QVariant{} : QVariant(v);
}

QVariant QtAccessible::minimumValue() const {
  if (!isValid()) {
    return {};
  }
  const double v = mAcc->MinValue();
  return std::isnan(v) ? QVariant{} : QVariant(v);
}

QVariant QtAccessible::minimumStepSize() const {
  if (!isValid()) {
    return {};
  }
  const double v = mAcc->Step();
  return std::isnan(v) ? QVariant{} : QVariant(v);
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

QAccessibleInterface* QtAccessible::caption() const {
  TableAccessible* table = AsTable();
  if (!table) {
    return nullptr;
  }
  return InterfaceFor(table->Caption());
}

QAccessibleInterface* QtAccessible::summary() const {
  // Gecko exposes summary as a string, not a separate accessible.
  return nullptr;
}

QAccessibleInterface* QtAccessible::cellAt(int row, int column) const {
  TableAccessible* table = AsTable();
  if (!table || row < 0 || column < 0) {
    return nullptr;
  }
  return InterfaceFor(table->CellAt(uint32_t(row), uint32_t(column)));
}

int QtAccessible::selectedCellCount() const {
  TableAccessible* table = AsTable();
  return table ? int(table->SelectedCellCount()) : 0;
}

QList<QAccessibleInterface*> QtAccessible::selectedCells() const {
  QList<QAccessibleInterface*> out;
  TableAccessible* table = AsTable();
  if (!table) {
    return out;
  }
  nsTArray<Accessible*> cells;
  table->SelectedCells(&cells);
  for (Accessible* cell : cells) {
    if (QAccessibleInterface* iface = InterfaceFor(cell)) {
      out.append(iface);
    }
  }
  return out;
}

QString QtAccessible::columnDescription(int column) const {
  TableAccessible* table = AsTable();
  if (!table || column < 0) {
    return {};
  }
  nsAutoString desc;
  table->ColDescription(uint32_t(column), desc);
  return ToQString(desc);
}

QString QtAccessible::rowDescription(int row) const {
  TableAccessible* table = AsTable();
  if (!table || row < 0) {
    return {};
  }
  nsAutoString desc;
  table->RowDescription(uint32_t(row), desc);
  return ToQString(desc);
}

int QtAccessible::selectedColumnCount() const {
  TableAccessible* table = AsTable();
  return table ? int(table->SelectedColCount()) : 0;
}

int QtAccessible::selectedRowCount() const {
  TableAccessible* table = AsTable();
  return table ? int(table->SelectedRowCount()) : 0;
}

int QtAccessible::columnCount() const {
  TableAccessible* table = AsTable();
  return table ? int(table->ColCount()) : 0;
}

int QtAccessible::rowCount() const {
  TableAccessible* table = AsTable();
  return table ? int(table->RowCount()) : 0;
}

QList<int> QtAccessible::selectedColumns() const {
  QList<int> out;
  TableAccessible* table = AsTable();
  if (!table) {
    return out;
  }
  nsTArray<uint32_t> cols;
  table->SelectedColIndices(&cols);
  for (uint32_t c : cols) {
    out.append(int(c));
  }
  return out;
}

QList<int> QtAccessible::selectedRows() const {
  QList<int> out;
  TableAccessible* table = AsTable();
  if (!table) {
    return out;
  }
  nsTArray<uint32_t> rows;
  table->SelectedRowIndices(&rows);
  for (uint32_t r : rows) {
    out.append(int(r));
  }
  return out;
}

bool QtAccessible::isColumnSelected(int column) const {
  TableAccessible* table = AsTable();
  return table && column >= 0 && table->IsColSelected(uint32_t(column));
}

bool QtAccessible::isRowSelected(int row) const {
  TableAccessible* table = AsTable();
  return table && row >= 0 && table->IsRowSelected(uint32_t(row));
}

bool QtAccessible::selectRow(int row) {
  TableAccessible* table = AsTable();
  if (!table || row < 0 || uint32_t(row) >= table->RowCount()) {
    return false;
  }
  const uint32_t cols = table->ColCount();
  bool any = false;
  for (uint32_t col = 0; col < cols; ++col) {
    if (Accessible* cell = table->CellAt(uint32_t(row), col)) {
      cell->SetSelected(true);
      any = true;
    }
  }
  return any;
}

bool QtAccessible::selectColumn(int column) {
  TableAccessible* table = AsTable();
  if (!table || column < 0 || uint32_t(column) >= table->ColCount()) {
    return false;
  }
  const uint32_t rows = table->RowCount();
  bool any = false;
  for (uint32_t row = 0; row < rows; ++row) {
    if (Accessible* cell = table->CellAt(row, uint32_t(column))) {
      cell->SetSelected(true);
      any = true;
    }
  }
  return any;
}

bool QtAccessible::unselectRow(int row) {
  TableAccessible* table = AsTable();
  if (!table || row < 0 || uint32_t(row) >= table->RowCount()) {
    return false;
  }
  const uint32_t cols = table->ColCount();
  bool any = false;
  for (uint32_t col = 0; col < cols; ++col) {
    if (Accessible* cell = table->CellAt(uint32_t(row), col)) {
      cell->SetSelected(false);
      any = true;
    }
  }
  return any;
}

bool QtAccessible::unselectColumn(int column) {
  TableAccessible* table = AsTable();
  if (!table || column < 0 || uint32_t(column) >= table->ColCount()) {
    return false;
  }
  const uint32_t rows = table->RowCount();
  bool any = false;
  for (uint32_t row = 0; row < rows; ++row) {
    if (Accessible* cell = table->CellAt(row, uint32_t(column))) {
      cell->SetSelected(false);
      any = true;
    }
  }
  return any;
}

void QtAccessible::modelChange(QAccessibleTableModelChangeEvent*) {}

// ---------------------------------------------------------------------------
// Table cell
// ---------------------------------------------------------------------------

bool QtAccessible::isSelected() const {
  TableCellAccessible* cell = AsTableCell();
  return cell && cell->Selected();
}

QList<QAccessibleInterface*> QtAccessible::columnHeaderCells() const {
  QList<QAccessibleInterface*> out;
  TableCellAccessible* cell = AsTableCell();
  if (!cell) {
    return out;
  }
  nsTArray<Accessible*> headers;
  cell->ColHeaderCells(&headers);
  for (Accessible* h : headers) {
    if (QAccessibleInterface* iface = InterfaceFor(h)) {
      out.append(iface);
    }
  }
  return out;
}

QList<QAccessibleInterface*> QtAccessible::rowHeaderCells() const {
  QList<QAccessibleInterface*> out;
  TableCellAccessible* cell = AsTableCell();
  if (!cell) {
    return out;
  }
  nsTArray<Accessible*> headers;
  cell->RowHeaderCells(&headers);
  for (Accessible* h : headers) {
    if (QAccessibleInterface* iface = InterfaceFor(h)) {
      out.append(iface);
    }
  }
  return out;
}

int QtAccessible::columnIndex() const {
  TableCellAccessible* cell = AsTableCell();
  return cell ? int(cell->ColIdx()) : -1;
}

int QtAccessible::rowIndex() const {
  TableCellAccessible* cell = AsTableCell();
  return cell ? int(cell->RowIdx()) : -1;
}

int QtAccessible::columnExtent() const {
  TableCellAccessible* cell = AsTableCell();
  return cell ? int(cell->ColExtent()) : 1;
}

int QtAccessible::rowExtent() const {
  TableCellAccessible* cell = AsTableCell();
  return cell ? int(cell->RowExtent()) : 1;
}

QAccessibleInterface* QtAccessible::table() const {
  TableCellAccessible* cell = AsTableCell();
  if (!cell) {
    return nullptr;
  }
  TableAccessible* table = cell->Table();
  return table ? InterfaceFor(table->AsAccessible()) : nullptr;
}

// ---------------------------------------------------------------------------
// Factory / lifecycle
// ---------------------------------------------------------------------------

static bool sFactoryInstalled = false;

QAccessibleInterface* a11y::QtWindowAccessibleFactory(const QString& className,
                                                      QObject* object) {
  if (!object) {
    return nullptr;
  }
  // QtWindowBridge is our QWindow subclass (Q_OBJECT).
  if (className != QLatin1String("QtWindowBridge") &&
      !object->inherits("QtWindowBridge")) {
    return nullptr;
  }
  auto* bridge = static_cast<QtWindowBridge*>(object);
  nsWindow* win = bridge->GetWidget();
  if (!win) {
    return nullptr;
  }
  LocalAccessible* root = win->GetRootAccessible();
  if (!root) {
    return nullptr;
  }
  auto* wrap = static_cast<AccessibleWrap*>(root);
  if (QtAccessible* existing = wrap->GetQtAccessible()) {
    existing->SetObject(object);
    // Already registered — return null so Qt keeps the cached entry; if none,
    // fall through by returning existing (Qt 6 tolerates re-query).
    return existing;
  }
  auto* iface = new QtAccessible(root, object);
  wrap->AdoptQtAccessible(iface);
  return iface;
}

void a11y::InitQtAccessibility() {
  if (!sFactoryInstalled) {
    QAccessible::installFactory(QtWindowAccessibleFactory);
    sFactoryInstalled = true;
  }
  QAccessible::setActive(true);
}

void a11y::ShutdownQtAccessibility() {
  // Leave factory installed; a11y may re-init. Deactivate.
  QAccessible::setActive(false);
}
