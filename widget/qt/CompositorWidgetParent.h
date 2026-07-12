/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_CompositorWidgetParent_h
#define widget_qt_CompositorWidgetParent_h

#include "QtCompositorWidget.h"
#include "mozilla/VsyncDispatcher.h"
#include "mozilla/widget/PCompositorWidgetParent.h"

namespace mozilla {
namespace widget {

class CompositorWidgetParent final : public PCompositorWidgetParent,
                                     public QtCompositorWidget {
 public:
  NS_INLINE_DECL_REFCOUNTING_INHERITED(CompositorWidgetParent, CompositorWidget)

  explicit CompositorWidgetParent(const CompositorWidgetInitData& aInitData,
                                  const layers::CompositorOptions& aOptions);

  void ActorDestroy(ActorDestroyReason aWhy) override {}

  void ObserveVsync(VsyncObserver* aObserver) override;
  RefPtr<VsyncObserver> GetVsyncObserver() const override;

  mozilla::ipc::IPCResult RecvNotifyClientSizeChanged(
      const LayoutDeviceIntSize& aClientSize) override;
  mozilla::ipc::IPCResult RecvCleanupResources() override;

 private:
  ~CompositorWidgetParent() override;
  RefPtr<VsyncObserver> mVsyncObserver;
};

}  // namespace widget
}  // namespace mozilla

#endif
