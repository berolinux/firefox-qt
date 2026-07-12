/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsDragService.h"

#include "mozilla/ClearOnShutdown.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/Logging.h"
#include "mozilla/PresShell.h"
#include "mozilla/StaticPtr.h"
#include "nsArrayUtils.h"
#include "nsComponentManagerUtils.h"
#include "nsDirectoryServiceDefs.h"
#include "nsEscape.h"
#include "nsIChannel.h"
#include "nsIDragService.h"
#include "nsIFile.h"
#include "nsIInputStream.h"
#include "nsIOutputStream.h"
#include "nsISupportsPrimitives.h"
#include "nsITransferable.h"
#include "nsIURI.h"
#include "nsIURL.h"
#include "nsNetUtil.h"
#include "nsPrimitiveHelpers.h"
#include "nsWindow.h"
#include "nsXPCOM.h"
#include "nsStringStream.h"

#pragma GCC visibility push(default)
#include <QDrag>
#include <QImage>
#include <QList>
#include <QMimeData>
#include <QPixmap>
#include <QPoint>
#include <QString>
#include <QUrl>
#include <QWindow>
#pragma GCC visibility pop

#include <cstdlib>

using namespace mozilla;
using namespace mozilla::gfx;

static LazyLogModule gQtDragLog("QtDrag");
#define LOGD(...) MOZ_LOG(gQtDragLog, LogLevel::Debug, (__VA_ARGS__))

static StaticRefPtr<nsDragService> sDragServiceInstance;

// ---------------------------------------------------------------------------
// Helpers: transferable ↔ QMimeData
// ---------------------------------------------------------------------------

namespace mozilla::widget::qt_dnd {

static QString ToQString(const nsAString& aStr) {
  return QString::fromUtf16(
      reinterpret_cast<const char16_t*>(aStr.BeginReading()),
      int(aStr.Length()));
}

static void FromQString(const QString& aSrc, nsAString& aDst) {
  aDst.Assign(reinterpret_cast<const char16_t*>(aSrc.utf16()), aSrc.size());
}

static bool GetStringFromTransferable(nsITransferable* aTrans,
                                      const char* aFlavor, nsAString& aOut) {
  aOut.Truncate();
  nsCOMPtr<nsISupports> data;
  if (NS_FAILED(aTrans->GetTransferData(aFlavor, getter_AddRefs(data))) ||
      !data) {
    return false;
  }
  nsCOMPtr<nsISupportsString> str(do_QueryInterface(data));
  if (!str) {
    return false;
  }
  str->GetData(aOut);
  return !aOut.IsEmpty();
}

static void SetUtf16MimeData(QMimeData* aMime, const char* aFlavor,
                             const nsAString& aText) {
  aMime->setData(
      QString::fromLatin1(aFlavor),
      QByteArray(reinterpret_cast<const char*>(aText.BeginReading()),
                 int(aText.Length() * sizeof(char16_t))));
}

static void SetBinaryMimeData(QMimeData* aMime, const char* aFlavor,
                              const void* aData, uint32_t aLen) {
  if (aData && aLen) {
    aMime->setData(QString::fromLatin1(aFlavor),
                   QByteArray(static_cast<const char*>(aData), int(aLen)));
  }
}

QMimeData* MimeDataFromTransferables(
    nsIArray* aTransferables, const nsTArray<nsString>& aExtraLocalPaths) {
  auto* mime = new QMimeData();
  QList<QUrl> urls;
  for (const auto& path : aExtraLocalPaths) {
    urls.append(QUrl::fromLocalFile(ToQString(path)));
  }

  if (!aTransferables) {
    if (!urls.isEmpty()) {
      mime->setUrls(urls);
    }
    return mime;
  }

  uint32_t count = 0;
  aTransferables->GetLength(&count);

  for (uint32_t i = 0; i < count; ++i) {
    nsCOMPtr<nsITransferable> trans = do_QueryElementAt(aTransferables, i);
    if (!trans) {
      continue;
    }
    nsTArray<nsCString> flavors;
    if (NS_FAILED(trans->FlavorsTransferableCanExport(flavors))) {
      continue;
    }
    for (const auto& flavor : flavors) {
      if (flavor.EqualsLiteral(kTextMime)) {
        nsAutoString text;
        if (GetStringFromTransferable(trans, kTextMime, text) &&
            !mime->hasText()) {
          mime->setText(ToQString(text));
        }
      } else if (flavor.EqualsLiteral(kHTMLMime) ||
                 flavor.EqualsLiteral(kNativeHTMLMime)) {
        nsAutoString html;
        if (GetStringFromTransferable(trans, flavor.get(), html)) {
          if (!mime->hasHtml()) {
            mime->setHtml(ToQString(html));
          }
          SetUtf16MimeData(mime, flavor.get(), html);
        }
      } else if (flavor.EqualsLiteral(kURLMime) ||
                 flavor.EqualsLiteral(kURLDataMime) ||
                 flavor.EqualsLiteral(kURLPrivateMime) ||
                 flavor.EqualsLiteral(kURLDescriptionMime) ||
                 flavor.EqualsLiteral(kFilePromiseURLMime)) {
        nsAutoString url;
        if (GetStringFromTransferable(trans, flavor.get(), url)) {
          SetUtf16MimeData(mime, flavor.get(), url);
          if (flavor.EqualsLiteral(kURLMime) ||
              flavor.EqualsLiteral(kURLDataMime) ||
              flavor.EqualsLiteral(kURLPrivateMime) ||
              flavor.EqualsLiteral(kFilePromiseURLMime)) {
            int32_t nl = url.FindChar(u'\n');
            nsAutoString urlOnly;
            if (nl >= 0) {
              urlOnly.Assign(Substring(url, 0, nl));
            } else {
              urlOnly = url;
            }
            QUrl qurl(ToQString(urlOnly));
            if (qurl.isValid() && !urls.contains(qurl)) {
              urls.append(qurl);
            }
            if (!mime->hasText()) {
              mime->setText(ToQString(urlOnly));
            }
          }
        }
      } else if (flavor.EqualsLiteral(kFilePromiseDestFilename)) {
        nsAutoString name;
        if (GetStringFromTransferable(trans, flavor.get(), name)) {
          SetUtf16MimeData(mime, flavor.get(), name);
        }
      } else if (flavor.EqualsLiteral(kFileMime)) {
        nsCOMPtr<nsISupports> data;
        if (NS_SUCCEEDED(
                trans->GetTransferData(kFileMime, getter_AddRefs(data))) &&
            data) {
          nsCOMPtr<nsIFile> file(do_QueryInterface(data));
          if (file) {
            nsAutoString path;
            if (NS_SUCCEEDED(file->GetPath(path))) {
              QUrl qurl = QUrl::fromLocalFile(ToQString(path));
              if (!urls.contains(qurl)) {
                urls.append(qurl);
              }
            }
          }
        }
      } else if (flavor.EqualsLiteral(kPNGImageMime) ||
                 flavor.EqualsLiteral(kNativeImageMime) ||
                 flavor.EqualsLiteral(kJPEGImageMime) ||
                 flavor.EqualsLiteral(kGIFImageMime)) {
        nsCOMPtr<nsISupports> data;
        if (NS_SUCCEEDED(
                trans->GetTransferData(flavor.get(), getter_AddRefs(data))) &&
            data) {
          void* binData = nullptr;
          uint32_t len = 0;
          nsPrimitiveHelpers::CreateDataFromPrimitive(
              nsDependentCString(flavor.get()), data, &binData, &len);
          if (binData && len) {
            const char* mimeType = flavor.EqualsLiteral(kJPEGImageMime)
                                       ? "image/jpeg"
                                       : flavor.EqualsLiteral(kGIFImageMime)
                                             ? "image/gif"
                                             : "image/png";
            mime->setData(QString::fromLatin1(mimeType),
                          QByteArray(static_cast<const char*>(binData),
                                     int(len)));
            // Also keep original Gecko flavor for internal drops.
            SetBinaryMimeData(mime, flavor.get(), binData, len);
            free(binData);
          }
        }
      } else if (flavor.EqualsLiteral(kCustomTypesMime) ||
                 flavor.EqualsLiteral(kFilePromiseMime) ||
                 flavor.EqualsLiteral(kFilePromiseDirectoryMime)) {
        // Opaque / internal flavors: copy as raw if available.
        nsCOMPtr<nsISupports> data;
        if (NS_SUCCEEDED(
                trans->GetTransferData(flavor.get(), getter_AddRefs(data))) &&
            data) {
          void* binData = nullptr;
          uint32_t len = 0;
          nsPrimitiveHelpers::CreateDataFromPrimitive(
              nsDependentCString(flavor.get()), data, &binData, &len);
          if (binData && len) {
            SetBinaryMimeData(mime, flavor.get(), binData, len);
            free(binData);
          } else {
            // Maybe string-typed.
            nsAutoString text;
            if (GetStringFromTransferable(trans, flavor.get(), text)) {
              SetUtf16MimeData(mime, flavor.get(), text);
            }
          }
        }
      } else {
        // Generic pass-through for other string flavors.
        nsAutoString text;
        if (GetStringFromTransferable(trans, flavor.get(), text)) {
          SetUtf16MimeData(mime, flavor.get(), text);
        }
      }
    }
  }
  if (!urls.isEmpty()) {
    mime->setUrls(urls);
  }
  return mime;
}

bool MimeSupportsFlavor(const QMimeData* aMime, const char* aFlavor) {
  if (!aMime || !aFlavor) {
    return false;
  }
  if (!strcmp(aFlavor, kTextMime)) {
    return aMime->hasText() || aMime->hasUrls();
  }
  if (!strcmp(aFlavor, kHTMLMime) || !strcmp(aFlavor, kNativeHTMLMime)) {
    return aMime->hasHtml() ||
           aMime->hasFormat(QString::fromLatin1(aFlavor));
  }
  if (!strcmp(aFlavor, kURLMime) || !strcmp(aFlavor, kURLDataMime) ||
      !strcmp(aFlavor, kURLPrivateMime) ||
      !strcmp(aFlavor, kURLDescriptionMime) ||
      !strcmp(aFlavor, kFilePromiseURLMime)) {
    return aMime->hasUrls() || aMime->hasFormat(QString::fromLatin1(aFlavor));
  }
  if (!strcmp(aFlavor, kFileMime)) {
    if (!aMime->hasUrls()) {
      return false;
    }
    for (const QUrl& u : aMime->urls()) {
      if (u.isLocalFile()) {
        return true;
      }
    }
    return false;
  }
  if (!strcmp(aFlavor, kPNGImageMime) || !strcmp(aFlavor, kJPEGImageMime) ||
      !strcmp(aFlavor, kGIFImageMime) || !strcmp(aFlavor, kNativeImageMime)) {
    return aMime->hasImage() || aMime->hasFormat(QStringLiteral("image/png")) ||
           aMime->hasFormat(QStringLiteral("image/jpeg")) ||
           aMime->hasFormat(QStringLiteral("image/gif")) ||
           aMime->hasFormat(QString::fromLatin1(aFlavor));
  }
  return aMime->hasFormat(QString::fromLatin1(aFlavor));
}

uint32_t MimeNumItems(const QMimeData* aMime) {
  if (!aMime) {
    return 0;
  }
  if (aMime->hasUrls()) {
    return uint32_t(std::max(1, int(aMime->urls().size())));
  }
  if (aMime->hasText() || aMime->hasHtml() || aMime->hasImage()) {
    return 1;
  }
  return aMime->formats().isEmpty() ? 0 : 1;
}

static already_AddRefed<nsISupportsString> MakeStringSupports(
    const nsAString& aText) {
  nsCOMPtr<nsISupportsString> data(
      do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID));
  if (data) {
    data->SetData(aText);
  }
  return data.forget();
}

static bool GetUtf16FromMime(const QMimeData* aMime, const char* aFlavor,
                             nsAString& aOut) {
  aOut.Truncate();
  QByteArray bytes = aMime->data(QString::fromLatin1(aFlavor));
  if (bytes.isEmpty() || (bytes.size() % 2) != 0) {
    return false;
  }
  aOut.Assign(reinterpret_cast<const char16_t*>(bytes.constData()),
              bytes.size() / 2);
  return !aOut.IsEmpty();
}

bool TransferableFromMimeData(const QMimeData* aMime, const char* aFlavor,
                              nsITransferable* aTransferable,
                              uint32_t aItemIndex) {
  if (!aMime || !aFlavor || !aTransferable) {
    return false;
  }

  if (!strcmp(aFlavor, kTextMime) && aMime->hasText()) {
    nsString text;
    FromQString(aMime->text(), text);
    nsCOMPtr<nsISupportsString> data = MakeStringSupports(text);
    return data &&
           NS_SUCCEEDED(aTransferable->SetTransferData(kTextMime, data));
  }

  if ((!strcmp(aFlavor, kHTMLMime) || !strcmp(aFlavor, kNativeHTMLMime)) &&
      (aMime->hasHtml() ||
       aMime->hasFormat(QString::fromLatin1(aFlavor)))) {
    nsString html;
    if (aMime->hasHtml()) {
      FromQString(aMime->html(), html);
    } else {
      GetUtf16FromMime(aMime, aFlavor, html);
    }
    nsCOMPtr<nsISupportsString> data = MakeStringSupports(html);
    return data &&
           NS_SUCCEEDED(aTransferable->SetTransferData(aFlavor, data));
  }

  if ((!strcmp(aFlavor, kURLMime) || !strcmp(aFlavor, kURLDataMime) ||
       !strcmp(aFlavor, kURLPrivateMime) ||
       !strcmp(aFlavor, kURLDescriptionMime) ||
       !strcmp(aFlavor, kFilePromiseURLMime))) {
    nsString url;
    if (GetUtf16FromMime(aMime, aFlavor, url)) {
      nsCOMPtr<nsISupportsString> data = MakeStringSupports(url);
      return data &&
             NS_SUCCEEDED(aTransferable->SetTransferData(aFlavor, data));
    }
    if (aMime->hasUrls()) {
      const QList<QUrl> urls = aMime->urls();
      if (int(aItemIndex) >= urls.size()) {
        return false;
      }
      FromQString(urls.at(int(aItemIndex)).toString(QUrl::FullyEncoded), url);
      if (!strcmp(aFlavor, kURLMime)) {
        url.Append('\n');
      }
      nsCOMPtr<nsISupportsString> data = MakeStringSupports(url);
      return data &&
             NS_SUCCEEDED(aTransferable->SetTransferData(aFlavor, data));
    }
  }

  if (!strcmp(aFlavor, kFileMime) && aMime->hasUrls()) {
    const QList<QUrl> urls = aMime->urls();
    if (int(aItemIndex) >= urls.size()) {
      return false;
    }
    const QUrl& u = urls.at(int(aItemIndex));
    if (!u.isLocalFile()) {
      return false;
    }
    nsCOMPtr<nsIFile> file;
    nsString path;
    FromQString(u.toLocalFile(), path);
    if (NS_FAILED(NS_NewLocalFile(path, getter_AddRefs(file))) || !file) {
      return false;
    }
    nsCOMPtr<nsISupports> supports = do_QueryInterface(file);
    return supports &&
           NS_SUCCEEDED(aTransferable->SetTransferData(kFileMime, supports));
  }

  QString imageFmt;
  if (!strcmp(aFlavor, kPNGImageMime) || !strcmp(aFlavor, kNativeImageMime)) {
    imageFmt = QStringLiteral("image/png");
  } else if (!strcmp(aFlavor, kJPEGImageMime)) {
    imageFmt = QStringLiteral("image/jpeg");
  } else if (!strcmp(aFlavor, kGIFImageMime)) {
    imageFmt = QStringLiteral("image/gif");
  }
  if (!imageFmt.isEmpty()) {
    QByteArray bytes = aMime->hasFormat(imageFmt)
                           ? aMime->data(imageFmt)
                           : aMime->data(QString::fromLatin1(aFlavor));
    if (!bytes.isEmpty()) {
      nsCOMPtr<nsISupports> genericDataWrapper;
      nsPrimitiveHelpers::CreatePrimitiveForData(
          nsDependentCString(aFlavor), bytes.constData(),
          uint32_t(bytes.size()), getter_AddRefs(genericDataWrapper));
      return genericDataWrapper &&
             NS_SUCCEEDED(
                 aTransferable->SetTransferData(aFlavor, genericDataWrapper));
    }
  }

  // Generic: raw bytes or UTF-16 stored under the flavor name.
  if (aMime->hasFormat(QString::fromLatin1(aFlavor))) {
    QByteArray bytes = aMime->data(QString::fromLatin1(aFlavor));
    if (bytes.isEmpty()) {
      return false;
    }
    if ((bytes.size() % 2) == 0) {
      nsString text;
      text.Assign(reinterpret_cast<const char16_t*>(bytes.constData()),
                  bytes.size() / 2);
      // Prefer string if it looks like UTF-16 text (no NULs mid-string often).
      nsCOMPtr<nsISupportsString> asString = MakeStringSupports(text);
      if (asString &&
          NS_SUCCEEDED(aTransferable->SetTransferData(aFlavor, asString))) {
        return true;
      }
    }
    nsCOMPtr<nsISupports> generic;
    nsPrimitiveHelpers::CreatePrimitiveForData(
        nsDependentCString(aFlavor), bytes.constData(), uint32_t(bytes.size()),
        getter_AddRefs(generic));
    return generic &&
           NS_SUCCEEDED(aTransferable->SetTransferData(aFlavor, generic));
  }

  return false;
}

}  // namespace mozilla::widget::qt_dnd

// ---------------------------------------------------------------------------
// nsDragService / nsDragSession
// ---------------------------------------------------------------------------

already_AddRefed<nsDragService> nsDragService::GetInstance() {
  if (!sDragServiceInstance) {
    sDragServiceInstance = new nsDragService();
    ClearOnShutdown(&sDragServiceInstance);
  }
  return do_AddRef(sDragServiceInstance);
}

already_AddRefed<nsIDragSession> nsDragService::CreateDragSession() {
  return do_AddRef(new nsDragSession());
}

nsDragSession::~nsDragSession() { CleanupTempFiles(); }

void nsDragSession::CleanupTempFiles() {
  for (int32_t i = 0; i < mTemporaryDirs.Count(); ++i) {
    if (nsIFile* dir = mTemporaryDirs.ObjectAt(i)) {
      (void)dir->Remove(/* recursive */ true);
    }
  }
  mTemporaryDirs.Clear();
}

void nsDragSession::SetDropMimeData(const QMimeData* aMime) {
  mDropMime.reset(new QMimeData());
  if (!aMime) {
    return;
  }
  for (const QString& format : aMime->formats()) {
    mDropMime->setData(format, aMime->data(format));
  }
}

NS_IMETHODIMP
nsDragSession::GetNumDropItems(uint32_t* aNumItems) {
  if (!aNumItems) {
    return NS_ERROR_INVALID_ARG;
  }
  if (mDropMime) {
    *aNumItems = mozilla::widget::qt_dnd::MimeNumItems(mDropMime.get());
    return NS_OK;
  }
  if (mSourceTransferables) {
    mSourceTransferables->GetLength(aNumItems);
    return NS_OK;
  }
  *aNumItems = 0;
  return NS_OK;
}

NS_IMETHODIMP
nsDragSession::IsDataFlavorSupported(const char* aDataFlavor, bool* _retval) {
  if (!_retval) {
    return NS_ERROR_INVALID_ARG;
  }
  *_retval = false;
  if (!aDataFlavor) {
    return NS_OK;
  }

  if (mDropMime) {
    *_retval = mozilla::widget::qt_dnd::MimeSupportsFlavor(mDropMime.get(),
                                                           aDataFlavor);
    return NS_OK;
  }

  if (mSourceTransferables) {
    uint32_t count = 0;
    mSourceTransferables->GetLength(&count);
    for (uint32_t i = 0; i < count; ++i) {
      nsCOMPtr<nsITransferable> trans =
          do_QueryElementAt(mSourceTransferables, i);
      if (!trans) {
        continue;
      }
      nsTArray<nsCString> flavors;
      if (NS_FAILED(trans->FlavorsTransferableCanExport(flavors))) {
        continue;
      }
      for (const auto& f : flavors) {
        if (f.Equals(aDataFlavor)) {
          *_retval = true;
          return NS_OK;
        }
        // File promises also satisfy kFileMime after materialization.
        if (!strcmp(aDataFlavor, kFileMime) &&
            f.EqualsLiteral(kFilePromiseMime)) {
          *_retval = true;
          return NS_OK;
        }
      }
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsDragSession::GetData(nsITransferable* aTransferable, uint32_t aItemIndex) {
  if (!aTransferable) {
    return NS_ERROR_INVALID_ARG;
  }

  nsTArray<nsCString> flavors;
  if (NS_FAILED(aTransferable->FlavorsTransferableCanImport(flavors))) {
    return NS_ERROR_FAILURE;
  }

  if (mDropMime) {
    for (const auto& flavor : flavors) {
      if (mozilla::widget::qt_dnd::TransferableFromMimeData(
              mDropMime.get(), flavor.get(), aTransferable, aItemIndex)) {
        return NS_OK;
      }
    }
    return NS_ERROR_FAILURE;
  }

  if (mSourceTransferables) {
    nsCOMPtr<nsITransferable> src =
        do_QueryElementAt(mSourceTransferables, aItemIndex);
    if (!src) {
      src = do_QueryElementAt(mSourceTransferables, 0);
    }
    if (!src) {
      return NS_ERROR_FAILURE;
    }
    for (const auto& flavor : flavors) {
      nsCOMPtr<nsISupports> data;
      if (NS_SUCCEEDED(
              src->GetTransferData(flavor.get(), getter_AddRefs(data))) &&
          data) {
        aTransferable->SetTransferData(flavor.get(), data);
        return NS_OK;
      }
    }
  }
  return NS_ERROR_FAILURE;
}

// ---------------------------------------------------------------------------
// File promises → temp files (so other apps receive real file:// URLs)
// ---------------------------------------------------------------------------

static nsresult GetDownloadDetails(nsITransferable* aTransferable,
                                   nsIURI** aSourceURI, nsAString& aFilename) {
  *aSourceURI = nullptr;
  nsCOMPtr<nsISupports> urlPrimitive;
  nsresult rv = aTransferable->GetTransferData(kFilePromiseURLMime,
                                               getter_AddRefs(urlPrimitive));
  if (NS_FAILED(rv)) {
    return NS_ERROR_FAILURE;
  }
  nsCOMPtr<nsISupportsString> srcUrlPrimitive = do_QueryInterface(urlPrimitive);
  if (!srcUrlPrimitive) {
    return NS_ERROR_FAILURE;
  }
  nsAutoString srcUri;
  srcUrlPrimitive->GetData(srcUri);
  if (srcUri.IsEmpty()) {
    return NS_ERROR_FAILURE;
  }
  nsCOMPtr<nsIURI> sourceURI;
  if (NS_FAILED(NS_NewURI(getter_AddRefs(sourceURI), srcUri))) {
    return NS_ERROR_FAILURE;
  }

  nsAutoString srcFileName;
  nsCOMPtr<nsISupports> fileNamePrimitive;
  rv = aTransferable->GetTransferData(kFilePromiseDestFilename,
                                      getter_AddRefs(fileNamePrimitive));
  if (NS_SUCCEEDED(rv)) {
    nsCOMPtr<nsISupportsString> nameStr = do_QueryInterface(fileNamePrimitive);
    if (nameStr) {
      nameStr->GetData(srcFileName);
    }
  }
  if (srcFileName.IsEmpty()) {
    nsCOMPtr<nsIURL> sourceURL = do_QueryInterface(sourceURI);
    if (sourceURL) {
      nsAutoCString urlFileName;
      sourceURL->GetFileName(urlFileName);
      NS_UnescapeURL(urlFileName);
      CopyUTF8toUTF16(urlFileName, srcFileName);
    }
  }
  if (srcFileName.IsEmpty()) {
    srcFileName.AssignLiteral("download");
  }

  sourceURI.forget(aSourceURI);
  aFilename = srcFileName;
  return NS_OK;
}

void nsDragSession::MaterializeFilePromises(nsIArray* aTransferables,
                                            nsTArray<nsString>& aLocalPathsOut) {
  if (!aTransferables) {
    return;
  }
  uint32_t count = 0;
  aTransferables->GetLength(&count);
  for (uint32_t i = 0; i < count; ++i) {
    nsCOMPtr<nsITransferable> item = do_QueryElementAt(aTransferables, i);
    if (!item) {
      continue;
    }
    nsTArray<nsCString> flavors;
    if (NS_FAILED(item->FlavorsTransferableCanExport(flavors))) {
      continue;
    }
    bool isPromise = false;
    for (const auto& f : flavors) {
      if (f.EqualsLiteral(kFilePromiseMime) ||
          f.EqualsLiteral(kFilePromiseURLMime)) {
        isPromise = true;
        break;
      }
    }
    if (!isPromise) {
      continue;
    }

    nsCOMPtr<nsIURI> sourceURI;
    nsAutoString wideFileName;
    if (NS_FAILED(GetDownloadDetails(item, getter_AddRefs(sourceURI),
                                     wideFileName))) {
      LOGD("file promise: missing url/name");
      continue;
    }

    nsCOMPtr<nsIFile> tmpDir;
    if (NS_FAILED(
            NS_GetSpecialDirectory(NS_OS_TEMP_DIR, getter_AddRefs(tmpDir)))) {
      continue;
    }
    tmpDir->Append(u"dnd_file"_ns);
    if (NS_FAILED(tmpDir->CreateUnique(nsIFile::DIRECTORY_TYPE, 0700))) {
      continue;
    }
    mTemporaryDirs.AppendObject(tmpDir);

    nsCOMPtr<nsIFile> outFile;
    tmpDir->Clone(getter_AddRefs(outFile));
    outFile->Append(wideFileName);

    nsCOMPtr<nsIPrincipal> principal = item->GetDataPrincipal();
    nsContentPolicyType contentPolicyType = item->GetContentPolicyType();
    nsCOMPtr<nsICookieJarSettings> cookieJarSettings =
        item->GetCookieJarSettings();
    nsCOMPtr<nsIChannel> channel;
    nsresult rv = NS_NewChannel(
        getter_AddRefs(channel), sourceURI, principal,
        nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
        contentPolicyType, cookieJarSettings);
    if (NS_FAILED(rv)) {
      LOGD("file promise: channel create failed");
      continue;
    }
    nsCOMPtr<nsIInputStream> inputStream;
    if (NS_FAILED(channel->Open(getter_AddRefs(inputStream)))) {
      LOGD("file promise: channel open failed");
      continue;
    }
    nsCOMPtr<nsIOutputStream> outputStream;
    if (NS_FAILED(NS_NewLocalFileOutputStream(getter_AddRefs(outputStream),
                                              outFile))) {
      continue;
    }
    char buffer[8192];
    uint32_t readCount = 0;
    uint32_t writeCount = 0;
    bool ok = true;
    while (true) {
      if (NS_FAILED(inputStream->Read(buffer, sizeof(buffer), &readCount))) {
        ok = false;
        break;
      }
      if (readCount == 0) {
        break;
      }
      if (NS_FAILED(outputStream->Write(buffer, readCount, &writeCount)) ||
          writeCount != readCount) {
        ok = false;
        break;
      }
    }
    inputStream->Close();
    outputStream->Close();
    if (!ok) {
      LOGD("file promise: copy failed");
      continue;
    }

    nsAutoString path;
    if (NS_SUCCEEDED(outFile->GetPath(path))) {
      aLocalPathsOut.AppendElement(path);
      LOGD("file promise materialized");
    }
  }
}

// ---------------------------------------------------------------------------
// Drag feedback pixmap
// ---------------------------------------------------------------------------

bool nsDragSession::ConstructDragPixmap(
    const Maybe<CSSIntRegion>& aRegion, QPixmap* aOutPixmap, int* aHotspotX,
    int* aHotspotY) {
  if (!aOutPixmap || !aHotspotX || !aHotspotY) {
    return false;
  }
  if (!mHasImage && !mSelection && !mSourceNode) {
    return false;
  }

  LayoutDeviceIntRect dragRect;
  nsPresContext* pc = nullptr;
  RefPtr<SourceSurface> surface;
  nsresult rv =
      DrawDrag(mSourceNode, aRegion.isSome() ? aRegion : mRegion,
               mScreenPosition, &dragRect, &surface, &pc);
  if (NS_FAILED(rv) || !surface || !pc || dragRect.IsEmpty()) {
    LOGD("DrawDrag failed or empty");
    return false;
  }

  const auto screenPoint =
      LayoutDeviceIntPoint::Round(mScreenPosition * pc->CSSToDevPixelScale());
  *aHotspotX = screenPoint.x - dragRect.x;
  *aHotspotY = screenPoint.y - dragRect.y;

  const int width = dragRect.width;
  const int height = dragRect.height;
  RefPtr<DataSourceSurface> dataSurface = Factory::CreateDataSourceSurface(
      IntSize(width, height), SurfaceFormat::B8G8R8A8);
  if (!dataSurface) {
    return false;
  }
  DataSourceSurface::MappedSurface map;
  if (!dataSurface->Map(DataSourceSurface::MapType::READ_WRITE, &map)) {
    return false;
  }
  {
    RefPtr<DrawTarget> dt = Factory::CreateDrawTargetForData(
        BackendType::CAIRO, map.mData, dataSurface->GetSize(), map.mStride,
        dataSurface->GetFormat());
    if (!dt) {
      dataSurface->Unmap();
      return false;
    }
    dt->FillRect(gfx::Rect(0, 0, width, height),
                 SurfacePattern(surface, ExtendMode::CLAMP),
                 DrawOptions(1.0f, CompositionOp::OP_SOURCE));
  }

  // Convert BGRA → RGBA QImage with overall translucency (matches other
  // platforms' drag ghost).
  QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
  for (int y = 0; y < height; ++y) {
    const uint8_t* src = map.mData + y * map.mStride;
    QRgb* dest = reinterpret_cast<QRgb*>(image.scanLine(y));
    for (int x = 0; x < width; ++x) {
      const uint8_t b = src[0];
      const uint8_t g = src[1];
      const uint8_t r = src[2];
      const uint8_t a = src[3];
      const uint8_t na = uint8_t(a * DRAG_TRANSLUCENCY);
      const uint8_t nr = uint8_t(r * DRAG_TRANSLUCENCY);
      const uint8_t ng = uint8_t(g * DRAG_TRANSLUCENCY);
      const uint8_t nb = uint8_t(b * DRAG_TRANSLUCENCY);
      dest[x] = qRgba(nr, ng, nb, na);
      src += 4;
    }
  }
  dataSurface->Unmap();

  *aOutPixmap = QPixmap::fromImage(std::move(image));
  return !aOutPixmap->isNull();
}

// ---------------------------------------------------------------------------
// Invoke
// ---------------------------------------------------------------------------

nsresult nsDragSession::InvokeDragSessionImpl(
    nsIWidget* aWidget, nsIArray* anArrayTransferables,
    const Maybe<CSSIntRegion>& aRegion, uint32_t aActionType) {
  LOGD("InvokeDragSessionImpl actions=0x%x", aActionType);

  mSourceTransferables = anArrayTransferables;
  mDropMime.reset();

  nsTArray<nsString> promisePaths;
  MaterializeFilePromises(anArrayTransferables, promisePaths);

  QMimeData* mime = mozilla::widget::qt_dnd::MimeDataFromTransferables(
      anArrayTransferables, promisePaths);
  if (!mime || mime->formats().isEmpty()) {
    delete mime;
    mSourceTransferables = nullptr;
    CleanupTempFiles();
    return NS_ERROR_FAILURE;
  }

  QObject* sourceObj = nullptr;
  if (nsWindow* win = nsWindow::FromWidget(aWidget)) {
    sourceObj = win->GetQtWindow();
  }

  QDrag drag(sourceObj);
  drag.setMimeData(mime);  // takes ownership of mime

  QPixmap pixmap;
  int hotX = 0, hotY = 0;
  if (ConstructDragPixmap(aRegion, &pixmap, &hotX, &hotY)) {
    drag.setPixmap(pixmap);
    hotX = std::clamp(hotX, 0, std::max(0, pixmap.width() - 1));
    hotY = std::clamp(hotY, 0, std::max(0, pixmap.height() - 1));
    drag.setHotSpot(QPoint(hotX, hotY));
    LOGD("drag pixmap %dx%d hotspot %d,%d", pixmap.width(), pixmap.height(),
         hotX, hotY);
  }

  OpenDragPopup();

  Qt::DropActions actions = Qt::IgnoreAction;
  if (aActionType & nsIDragService::DRAGDROP_ACTION_COPY) {
    actions |= Qt::CopyAction;
  }
  if (aActionType & nsIDragService::DRAGDROP_ACTION_MOVE) {
    actions |= Qt::MoveAction;
  }
  if (aActionType & nsIDragService::DRAGDROP_ACTION_LINK) {
    actions |= Qt::LinkAction;
  }
  if (actions == Qt::IgnoreAction) {
    actions = Qt::CopyAction;
  }

  const Qt::DropAction result = drag.exec(actions, Qt::CopyAction);
  LOGD("QDrag finished action=%d", int(result));

  SetDragEndPoint(LayoutDeviceIntPoint(0, 0));
  (void)EndDragSession(result != Qt::IgnoreAction, 0);

  mSourceTransferables = nullptr;
  // Keep temp files briefly? Other apps may still be reading. Schedule cleanup
  // on session destruction (when the next session starts or process exits).
  // For long-lived files, destruction of the session cleans them up.
  return NS_OK;
}
