/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_NativeKeyBindings_h
#define widget_qt_NativeKeyBindings_h

#include "mozilla/EventForwards.h"
#include "mozilla/Maybe.h"
#include "NativeKeyBindingsType.h"
#include "nsTArray.h"

namespace mozilla {
class WritingMode;

namespace widget {

// Linux/Qt edit commands mirroring GTK's default text bindings (without
// querying a live GtkWidget / QWidget shortcut map).
class NativeKeyBindings final {
 public:
  static NativeKeyBindings* GetInstance(NativeKeyBindingsType aType);
  static void Shutdown();
  static void GetEditCommandsForTests(NativeKeyBindingsType aType,
                                      const WidgetKeyboardEvent& aEvent,
                                      const Maybe<WritingMode>& aWritingMode,
                                      nsTArray<CommandInt>& aCommands);

  void GetEditCommands(const WidgetKeyboardEvent& aEvent,
                       const Maybe<WritingMode>& aWritingMode,
                       nsTArray<CommandInt>& aCommands);

 private:
  explicit NativeKeyBindings(NativeKeyBindingsType aType) : mType(aType) {}

  NativeKeyBindingsType mType;

  static NativeKeyBindings* sInstanceForSingleLineEditor;
  static NativeKeyBindings* sInstanceForMultiLineEditor;
};

}  // namespace widget
}  // namespace mozilla

#endif
