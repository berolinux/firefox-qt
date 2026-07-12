/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsIconChannel.h"

#include <algorithm>

#include "mozilla/CheckedInt.h"
#include "mozilla/gfx/Swizzle.h"
#include "mozilla/ipc/ByteBuf.h"
#include "mozilla/dom/ContentChild.h"
#include "nsIIconURI.h"
#include "nsIURL.h"
#include "nsIStringStream.h"
#include "nsMimeTypes.h"
#include "nsNetUtil.h"
#include "nsComponentManagerUtils.h"
#include "nsIPipe.h"
#include "nsIAsyncInputStream.h"
#include "nsIAsyncOutputStream.h"
#include "gfxPlatform.h"
#include "nsXULAppAPI.h"

#pragma GCC visibility push(default)
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QString>
#pragma GCC visibility pop

using namespace mozilla;
using mozilla::ipc::ByteBuf;

NS_IMPL_ISUPPORTS(nsIconChannel, nsIRequest, nsIChannel)

static nsresult QImageToByteBuf(const QImage& aImage, ByteBuf* aByteBuf) {
  if (aImage.isNull() || aImage.width() <= 0 || aImage.height() <= 0 ||
      aImage.width() >= 256 || aImage.height() >= 256) {
    return NS_ERROR_UNEXPECTED;
  }

  QImage converted = aImage.convertToFormat(QImage::Format_RGBA8888);
  const int width = converted.width();
  const int height = converted.height();
  const int n_channels = 4;
  CheckedInt32 buf_size = 4 + n_channels * CheckedInt32(height) * CheckedInt32(width);
  if (!buf_size.isValid()) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  uint8_t* const buf = (uint8_t*)moz_xmalloc(buf_size.value());
  uint8_t* out = buf;
  *(out++) = uint8_t(width);
  *(out++) = uint8_t(height);
  *(out++) = uint8_t(mozilla::gfx::SurfaceFormat::OS_RGBA);
  *(out++) = 0xFF;

  const int outstride = width * n_channels;
  mozilla::gfx::SwizzleData(
      converted.constBits(), converted.bytesPerLine(),
      mozilla::gfx::SurfaceFormat::R8G8B8A8, out, outstride,
      mozilla::gfx::SurfaceFormat::OS_RGBA, mozilla::gfx::IntSize(width, height));

  *aByteBuf = ByteBuf(buf, buf_size.value(), buf_size.value());
  return NS_OK;
}

static QImage PlaceholderIcon(int aSize) {
  QImage image(aSize, aSize, QImage::Format_RGBA8888);
  image.fill(QColor(160, 160, 160, 255));
  return image;
}

static nsresult RenderIcon(const QIcon& aIcon, int aSize, ByteBuf* aDataOut) {
  QPixmap pixmap = aIcon.pixmap(aSize, aSize);
  if (pixmap.isNull()) {
    return QImageToByteBuf(PlaceholderIcon(aSize), aDataOut);
  }
  return QImageToByteBuf(pixmap.toImage(), aDataOut);
}

/* static */
nsresult nsIconChannel::GetIcon(nsIURI* aURI, ByteBuf* aDataOut) {
  nsCOMPtr<nsIMozIconURI> iconURI = do_QueryInterface(aURI);
  if (!iconURI) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  if (gfxPlatform::IsHeadless()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  const int iconSize = std::max(1, std::min(255, int(iconURI->GetImageSize())));

  nsAutoCString stockIcon;
  iconURI->GetStockIcon(stockIcon);
  if (!stockIcon.IsEmpty()) {
    QIcon icon = QIcon::fromTheme(QString::fromUtf8(stockIcon.get()));
    if (icon.isNull()) {
      icon = QIcon::fromTheme(QStringLiteral("text-x-generic"));
    }
    return RenderIcon(icon, iconSize, aDataOut);
  }

  nsAutoCString fileExt;
  iconURI->GetFileExtension(fileExt);
  if (!fileExt.IsEmpty()) {
    QString name = QStringLiteral("file.") + QString::fromUtf8(fileExt.get());
    QFileIconProvider provider;
    QIcon icon = provider.icon(QFileInfo(name));
    return RenderIcon(icon, iconSize, aDataOut);
  }

  nsCOMPtr<nsIURL> fileURL;
  iconURI->GetIconURL(getter_AddRefs(fileURL));
  if (fileURL) {
    nsAutoCString path;
    if (NS_SUCCEEDED(fileURL->GetFilePath(path)) && !path.IsEmpty()) {
      QFileIconProvider provider;
      QIcon icon = provider.icon(QFileInfo(QString::fromUtf8(path.get())));
      return RenderIcon(icon, iconSize, aDataOut);
    }
  }

  return RenderIcon(QIcon::fromTheme(QStringLiteral("text-x-generic")), iconSize,
                    aDataOut);
}

static nsresult ByteBufToStream(ByteBuf&& aBuf, nsIInputStream** aStream) {
  nsresult rv;
  nsCOMPtr<nsIStringInputStream> stream =
      do_CreateInstance("@mozilla.org/io/string-input-stream;1", &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }
  rv = stream->AdoptData(reinterpret_cast<char*>(aBuf.mData), aBuf.mLen);
  aBuf.mData = nullptr;
  NS_ENSURE_SUCCESS(rv, rv);
  stream.forget(aStream);
  return NS_OK;
}

nsresult nsIconChannel::Init(nsIURI* aURI, nsILoadInfo* aLoadInfo) {
  nsCOMPtr<nsIInputStream> stream;

  using ContentChild = mozilla::dom::ContentChild;
  if (auto* contentChild = ContentChild::GetSingleton()) {
    RefPtr<ContentChild::GetSystemIconPromise> icon =
        contentChild->SendGetSystemIcon(aURI);
    if (!icon) {
      return NS_ERROR_UNEXPECTED;
    }

    nsCOMPtr<nsIAsyncInputStream> inputStream;
    nsCOMPtr<nsIAsyncOutputStream> outputStream;
    NS_NewPipe2(getter_AddRefs(inputStream), getter_AddRefs(outputStream), true,
                false, 0, UINT32_MAX);

    icon->Then(
        mozilla::GetCurrentSerialEventTarget(), __func__,
        [outputStream](std::tuple<nsresult, mozilla::Maybe<ByteBuf>>&& aArg) {
          nsresult rv = std::get<0>(aArg);
          mozilla::Maybe<ByteBuf> bytes = std::move(std::get<1>(aArg));
          if (NS_SUCCEEDED(rv)) {
            MOZ_RELEASE_ASSERT(bytes);
            uint32_t written;
            rv = outputStream->Write(reinterpret_cast<char*>(bytes->mData),
                                     static_cast<uint32_t>(bytes->mLen),
                                     &written);
            if (NS_SUCCEEDED(rv)) {
              const bool wroteAll = static_cast<size_t>(written) == bytes->mLen;
              MOZ_ASSERT(wroteAll);
              if (!wroteAll) {
                rv = NS_ERROR_UNEXPECTED;
              }
            }
          } else {
            MOZ_ASSERT(!bytes);
          }
          if (NS_FAILED(rv)) {
            outputStream->CloseWithStatus(rv);
          }
        },
        [outputStream](mozilla::ipc::ResponseRejectReason) {
          outputStream->CloseWithStatus(NS_ERROR_FAILURE);
        });

    stream = inputStream.forget();
  } else {
    ByteBuf bytebuf;
    nsresult rv = GetIcon(aURI, &bytebuf);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = ByteBufToStream(std::move(bytebuf), getter_AddRefs(stream));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_NewInputStreamChannelInternal(
      getter_AddRefs(mRealChannel), aURI, stream.forget(),
      nsLiteralCString(IMAGE_ICON_MS), /* aContentCharset */ ""_ns, aLoadInfo);
}

void nsIconChannel::Shutdown() {}
