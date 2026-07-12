/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "NativeKeyBindings.h"

#include "mozilla/TextEvents.h"
#include "mozilla/WritingModes.h"

namespace mozilla::widget {

NativeKeyBindings* NativeKeyBindings::sInstanceForSingleLineEditor = nullptr;
NativeKeyBindings* NativeKeyBindings::sInstanceForMultiLineEditor = nullptr;

// static
NativeKeyBindings* NativeKeyBindings::GetInstance(NativeKeyBindingsType aType) {
  switch (aType) {
    case NativeKeyBindingsType::SingleLineEditor:
      if (!sInstanceForSingleLineEditor) {
        sInstanceForSingleLineEditor =
            new NativeKeyBindings(NativeKeyBindingsType::SingleLineEditor);
      }
      return sInstanceForSingleLineEditor;
    case NativeKeyBindingsType::MultiLineEditor:
    case NativeKeyBindingsType::RichTextEditor:
    default:
      if (!sInstanceForMultiLineEditor) {
        sInstanceForMultiLineEditor =
            new NativeKeyBindings(NativeKeyBindingsType::MultiLineEditor);
      }
      return sInstanceForMultiLineEditor;
  }
}

// static
void NativeKeyBindings::Shutdown() {
  delete sInstanceForSingleLineEditor;
  sInstanceForSingleLineEditor = nullptr;
  delete sInstanceForMultiLineEditor;
  sInstanceForMultiLineEditor = nullptr;
}

// static
void NativeKeyBindings::GetEditCommandsForTests(
    NativeKeyBindingsType aType, const WidgetKeyboardEvent& aEvent,
    const Maybe<WritingMode>& aWritingMode, nsTArray<CommandInt>& aCommands) {
  GetInstance(aType)->GetEditCommands(aEvent, aWritingMode, aCommands);
}

void NativeKeyBindings::GetEditCommands(
    const WidgetKeyboardEvent& aEvent, const Maybe<WritingMode>& aWritingMode,
    nsTArray<CommandInt>& aCommands) {
  MOZ_DIAGNOSTIC_ASSERT(aEvent.IsTrusted());

  if (aEvent.IsAlt() || aEvent.IsMeta()) {
    return;
  }

  static const size_t kBackward = 0;
  static const size_t kForward = 1;
  const size_t extend = aEvent.IsShift() ? 1 : 0;

  // [direction] for delete: backward, forward
  static const Command sDeleteChar[] = {Command::DeleteCharBackward,
                                        Command::DeleteCharForward};
  static const Command sDeleteWord[] = {Command::DeleteWordBackward,
                                        Command::DeleteWordForward};
  static const Command sDeleteLine[] = {Command::DeleteToBeginningOfLine,
                                        Command::DeleteToEndOfLine};

  // [extend][direction]
  static const Command sMoveChar[][2] = {
      {Command::CharPrevious, Command::CharNext},
      {Command::SelectCharPrevious, Command::SelectCharNext}};
  static const Command sMoveWord[][2] = {
      {Command::WordPrevious, Command::WordNext},
      {Command::SelectWordPrevious, Command::SelectWordNext}};
  static const Command sMoveLine[][2] = {
      {Command::LinePrevious, Command::LineNext},
      {Command::SelectLinePrevious, Command::SelectLineNext}};
  static const Command sMoveLineEnd[][2] = {
      {Command::BeginLine, Command::EndLine},
      {Command::SelectBeginLine, Command::SelectEndLine}};
  static const Command sMovePage[][2] = {
      {Command::MovePageUp, Command::MovePageDown},
      {Command::SelectPageUp, Command::SelectPageDown}};
  static const Command sMoveBuffer[][2] = {
      {Command::MoveTop, Command::MoveBottom},
      {Command::SelectTop, Command::SelectBottom}};

  Command command = Command::DoNothing;
  const KeyNameIndex keyName =
      aWritingMode.isSome() ? aEvent.GetRemappedKeyNameIndex(aWritingMode.ref())
                            : aEvent.mKeyNameIndex;

  switch (keyName) {
    case KEY_NAME_INDEX_USE_STRING:
      switch (aEvent.PseudoCharCode()) {
        case 'a':
        case 'A':
          if (aEvent.IsControl()) {
            command = Command::SelectAll;
          }
          break;
        case 'c':
        case 'C':
          if (aEvent.IsControl() && !aEvent.IsShift()) {
            command = Command::Copy;
          }
          break;
        case 'u':
        case 'U':
          if (mType == NativeKeyBindingsType::SingleLineEditor &&
              aEvent.IsControl() && !aEvent.IsShift()) {
            command = sDeleteLine[kBackward];
          }
          break;
        case 'v':
        case 'V':
          if (aEvent.IsControl() && !aEvent.IsShift()) {
            command = Command::Paste;
          }
          break;
        case 'x':
        case 'X':
          if (aEvent.IsControl() && !aEvent.IsShift()) {
            command = Command::Cut;
          }
          break;
        case '/':
          if (aEvent.IsControl() && !aEvent.IsShift()) {
            command = Command::SelectAll;
          }
          break;
        default:
          break;
      }
      break;
    case KEY_NAME_INDEX_Insert:
      if (aEvent.IsControl() && !aEvent.IsShift()) {
        command = Command::Copy;
      } else if (aEvent.IsShift() && !aEvent.IsControl()) {
        command = Command::Paste;
      }
      break;
    case KEY_NAME_INDEX_Delete:
      if (aEvent.IsShift()) {
        command = Command::Cut;
        break;
      }
      [[fallthrough]];
    case KEY_NAME_INDEX_Backspace: {
      const size_t direction =
          keyName == KEY_NAME_INDEX_Delete ? kForward : kBackward;
      if (aEvent.IsControl() && aEvent.IsShift()) {
        command = sDeleteLine[direction];
      } else if (aEvent.IsControl()) {
        command = sDeleteWord[direction];
      } else {
        command = sDeleteChar[direction];
      }
      break;
    }
    case KEY_NAME_INDEX_ArrowLeft:
    case KEY_NAME_INDEX_ArrowRight: {
      const size_t direction =
          keyName == KEY_NAME_INDEX_ArrowRight ? kForward : kBackward;
      command = (aEvent.IsControl() ? sMoveWord : sMoveChar)[extend][direction];
      break;
    }
    case KEY_NAME_INDEX_ArrowUp:
    case KEY_NAME_INDEX_ArrowDown: {
      const size_t direction =
          keyName == KEY_NAME_INDEX_ArrowDown ? kForward : kBackward;
      command = sMoveLine[extend][direction];
      break;
    }
    case KEY_NAME_INDEX_Home:
    case KEY_NAME_INDEX_End: {
      const size_t direction =
          keyName == KEY_NAME_INDEX_End ? kForward : kBackward;
      command =
          (aEvent.IsControl() ? sMoveBuffer : sMoveLineEnd)[extend][direction];
      break;
    }
    case KEY_NAME_INDEX_PageUp:
    case KEY_NAME_INDEX_PageDown: {
      const size_t direction =
          keyName == KEY_NAME_INDEX_PageDown ? kForward : kBackward;
      command = sMovePage[extend][direction];
      break;
    }
    default:
      break;
  }

  if (command != Command::DoNothing) {
    aCommands.AppendElement(static_cast<CommandInt>(command));
  }
}

}  // namespace mozilla::widget
