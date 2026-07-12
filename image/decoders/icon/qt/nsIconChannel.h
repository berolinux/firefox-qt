/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_image_decoders_icon_qt_nsIconChannel_h
#define mozilla_image_decoders_icon_qt_nsIconChannel_h

#include "nsIChannel.h"
#include "nsIURI.h"
#include "nsCOMPtr.h"

namespace mozilla {
namespace ipc {
class ByteBuf;
}
}  // namespace mozilla

class nsIconChannel final : public nsIChannel {
 public:
  NS_DECL_ISUPPORTS
  NS_FORWARD_NSIREQUEST(mRealChannel->)
  NS_FORWARD_NSICHANNEL(mRealChannel->)

  nsIconChannel() = default;

  static void Shutdown();

  nsresult Init(nsIURI* aURI, nsILoadInfo* aLoadInfo);

  static nsresult GetIcon(nsIURI* aURI, mozilla::ipc::ByteBuf* aDataOut);

 private:
  ~nsIconChannel() = default;
  nsCOMPtr<nsIChannel> mRealChannel;
};

#endif
