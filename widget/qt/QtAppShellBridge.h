/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_QtAppShellBridge_h
#define widget_qt_QtAppShellBridge_h

#pragma GCC visibility push(default)
#include <QObject>
#pragma GCC visibility pop

class nsAppShell;

// QObject bridge so the Gecko appshell can wake the Qt event loop via signals.
class QtAppShellBridge final : public QObject {
  Q_OBJECT
 public:
  explicit QtAppShellBridge(nsAppShell* aAppShell, QObject* aParent = nullptr);
  ~QtAppShellBridge() override = default;

 public Q_SLOTS:
  void NativeEventCallback();
  void QuitApplication();

 Q_SIGNALS:
  void WakeEventLoop();
  void RequestQuit();

 private:
  nsAppShell* mAppShell;
};

#endif
