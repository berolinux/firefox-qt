/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_a11y_QtAccessible_h_
#define mozilla_a11y_QtAccessible_h_

#include "Accessible.h"
#include "mozilla/a11y/Role.h"

#pragma GCC visibility push(default)
#include <QAccessible>
#include <QAccessibleInterface>
#include <QObject>
#include <QString>
#include <QRect>
#include <QWindow>
#pragma GCC visibility pop

namespace mozilla {
namespace a11y {

/**
 * QAccessibleInterface bridging a Gecko Accessible (local or remote).
 * Also implements Action / Text / EditableText / Value / Table / TableCell
 * when applicable.
 */
class QtAccessible final : public QAccessibleInterface,
                           public QAccessibleActionInterface,
                           public QAccessibleTextInterface,
                           public QAccessibleEditableTextInterface,
                           public QAccessibleValueInterface,
                           public QAccessibleTableInterface,
                           public QAccessibleTableCellInterface {
 public:
  explicit QtAccessible(Accessible* aAcc, QObject* aObject = nullptr);
  // Not public destructor — destroyed via QAccessible::deleteAccessibleInterface

  Accessible* Acc() const { return mAcc; }
  void ClearAcc() { mAcc = nullptr; }
  void SetObject(QObject* aObject) { mObject = aObject; }

  // QAccessibleInterface
  bool isValid() const override;
  QObject* object() const override;
  QWindow* window() const override;
  QAccessibleInterface* childAt(int x, int y) const override;
  QAccessibleInterface* parent() const override;
  QAccessibleInterface* child(int index) const override;
  int childCount() const override;
  int indexOfChild(const QAccessibleInterface* child) const override;
  QString text(QAccessible::Text t) const override;
  void setText(QAccessible::Text t, const QString& text) override;
  QRect rect() const override;
  QAccessible::Role role() const override;
  QAccessible::State state() const override;
  QAccessibleInterface* focusChild() const override;
  void* interface_cast(QAccessible::InterfaceType type) override;

  // QAccessibleActionInterface
  QStringList actionNames() const override;
  QString localizedActionDescription(const QString& name) const override;
  void doAction(const QString& actionName) override;
  QStringList keyBindingsForAction(const QString& actionName) const override;

  // QAccessibleTextInterface
  void selection(int selectionIndex, int* startOffset,
                 int* endOffset) const override;
  int selectionCount() const override;
  void addSelection(int startOffset, int endOffset) override;
  void removeSelection(int selectionIndex) override;
  void setSelection(int selectionIndex, int startOffset,
                    int endOffset) override;
  int cursorPosition() const override;
  void setCursorPosition(int position) override;
  QString text(int startOffset, int endOffset) const override;
  int characterCount() const override;
  QRect characterRect(int offset) const override;
  int offsetAtPoint(const QPoint& point) const override;
  void scrollToSubstring(int startIndex, int endIndex) override;
  QString attributes(int offset, int* startOffset,
                     int* endOffset) const override;

  // QAccessibleEditableTextInterface
  void deleteText(int startOffset, int endOffset) override;
  void insertText(int offset, const QString& text) override;
  void replaceText(int startOffset, int endOffset,
                   const QString& text) override;

  // QAccessibleValueInterface
  QVariant currentValue() const override;
  void setCurrentValue(const QVariant& value) override;
  QVariant maximumValue() const override;
  QVariant minimumValue() const override;
  QVariant minimumStepSize() const override;

  // QAccessibleTableInterface
  QAccessibleInterface* caption() const override;
  QAccessibleInterface* summary() const override;
  QAccessibleInterface* cellAt(int row, int column) const override;
  int selectedCellCount() const override;
  QList<QAccessibleInterface*> selectedCells() const override;
  QString columnDescription(int column) const override;
  QString rowDescription(int row) const override;
  int selectedColumnCount() const override;
  int selectedRowCount() const override;
  int columnCount() const override;
  int rowCount() const override;
  QList<int> selectedColumns() const override;
  QList<int> selectedRows() const override;
  bool isColumnSelected(int column) const override;
  bool isRowSelected(int row) const override;
  bool selectRow(int row) override;
  bool selectColumn(int column) override;
  bool unselectRow(int row) override;
  bool unselectColumn(int column) override;
  void modelChange(QAccessibleTableModelChangeEvent* event) override;

  // QAccessibleTableCellInterface
  bool isSelected() const override;
  QList<QAccessibleInterface*> columnHeaderCells() const override;
  QList<QAccessibleInterface*> rowHeaderCells() const override;
  int columnIndex() const override;
  int rowIndex() const override;
  int columnExtent() const override;
  int rowExtent() const override;
  QAccessibleInterface* table() const override;

  static QAccessibleInterface* InterfaceFor(Accessible* aAcc);
  static QAccessible::Role RoleFor(mozilla::a11y::role aRole);
  static QAccessible::State StateFor(uint64_t aState);
  static QWindow* WindowFor(Accessible* aAcc);

 private:
  ~QtAccessible() override = default;

  HyperTextAccessibleBase* AsHyperText() const;
  TableAccessible* AsTable() const;
  TableCellAccessible* AsTableCell() const;
  bool HasTextInterface() const;
  bool HasValueInterface() const;
  bool HasActionInterface() const;
  bool HasTableInterface() const;
  bool HasTableCellInterface() const;

  Accessible* mAcc = nullptr;
  QObject* mObject = nullptr;
};

// Install factory / activate Qt a11y bridge.
void InitQtAccessibility();
void ShutdownQtAccessibility();
QAccessibleInterface* QtWindowAccessibleFactory(const QString& className,
                                                QObject* object);

}  // namespace a11y
}  // namespace mozilla

#endif
