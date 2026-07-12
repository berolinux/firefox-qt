/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_ScreenHelperQt_h
#define widget_qt_ScreenHelperQt_h

#include "mozilla/widget/ScreenManager.h"
#pragma GCC visibility push(default)
#include <QObject>
class QScreen;
#pragma GCC visibility pop

namespace mozilla::widget {

// Device-pixel density for Gecko (not the CSS scale). Wayland compositors
// sometimes report QScreen::physicalSize already multiplied by the scale
// factor; we correct for that when needed.
float PhysicalDpiForQScreen(QScreen* aScreen);

class ScreenHelperQt final : public QObject, public ScreenManager::Helper {
  Q_OBJECT
 public:
  ScreenHelperQt();
  ~ScreenHelperQt() override;

 private Q_SLOTS:
  void RefreshScreens();
};

}  // namespace mozilla::widget

#endif
