/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "InProcessQtCompositorWidget.h"
#include "HeadlessCompositorWidget.h"
#include "HeadlessWidget.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "VsyncDispatcher.h"
#include "nsWindow.h"

namespace mozilla::widget {

/* static */
RefPtr<CompositorWidget> CompositorWidget::CreateLocal(
    const CompositorWidgetInitData& aInitData,
    const layers::CompositorOptions& aOptions, nsIWidget* aWidget) {
  if (aInitData.type() ==
      CompositorWidgetInitData::THeadlessCompositorWidgetInitData) {
    return new HeadlessCompositorWidget(
        aInitData.get_HeadlessCompositorWidgetInitData(), aOptions,
        static_cast<HeadlessWidget*>(aWidget));
  }
  return new InProcessQtCompositorWidget(
      aInitData.get_QtCompositorWidgetInitData(), aOptions,
      nsWindow::FromWidget(aWidget));
}

InProcessQtCompositorWidget::InProcessQtCompositorWidget(
    const QtCompositorWidgetInitData& aInitData,
    const layers::CompositorOptions& aOptions, nsWindow* aWindow)
    : QtCompositorWidget(aInitData, aOptions, aWindow) {}

void InProcessQtCompositorWidget::ObserveVsync(VsyncObserver* aObserver) {
  // mWidget is cleared in CleanupResources() during nsWindow::Destroy; the
  // compositor thread may still call ObserveVsync while tearing down.
  if (!mWidget || mWidget->Destroyed()) {
    return;
  }
  if (RefPtr<CompositorVsyncDispatcher> cvd =
          mWidget->GetCompositorVsyncDispatcher()) {
    cvd->SetCompositorVsyncObserver(aObserver);
  }
}

}  // namespace mozilla::widget
