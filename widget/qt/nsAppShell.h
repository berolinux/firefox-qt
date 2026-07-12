/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_nsAppShell_h
#define widget_qt_nsAppShell_h

#include "nsBaseAppShell.h"

class QApplication;
class QtAppShellBridge;

class nsAppShell final : public nsBaseAppShell {
 public:
  nsAppShell() = default;

  nsresult Init();

  NS_IMETHOD Run() override;
  NS_IMETHOD Exit() override;

  void OnNativeEventCallback() { NativeEventCallback(); }
  void OnQuitApplication();

  static QApplication* GetQtApp();

  // Returns the existing QApplication, or null before nsAppShell::Init.
  // Does not construct one (early construction busy-spins the event loop).
  static QApplication* EnsureQtApp();

#ifdef MOZ_ENABLE_DBUS
  // Shared signature with GTK AsyncDBus helpers; unused on Qt (QtDBus).
  static void DBusConnectionCheck() {}
#endif

 protected:
  virtual ~nsAppShell();

  void ScheduleNativeEventCallback() override;
  bool ProcessNextNativeEvent(bool mayWait) override;

 private:
  // Owned; deleted in ~nsAppShell() where QtAppShellBridge is complete.
  // (Avoid UniquePtr here: nsAppShell.h is exported and only forward-declares
  // the bridge type.)
  QApplication* mApp = nullptr;
  QtAppShellBridge* mBridge = nullptr;
  bool mOwnsApp = false;
  bool mQuit = false;
};

#endif
