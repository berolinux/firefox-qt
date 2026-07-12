/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_InProcessQtCompositorWidget_h
#define widget_qt_InProcessQtCompositorWidget_h

#include "QtCompositorWidget.h"

namespace mozilla::widget {

class InProcessQtCompositorWidget final : public QtCompositorWidget {
 public:
  InProcessQtCompositorWidget(const QtCompositorWidgetInitData& aInitData,
                              const layers::CompositorOptions& aOptions,
                              nsWindow* aWindow);

  void ObserveVsync(VsyncObserver* aObserver) override;
};

}  // namespace mozilla::widget

#endif
