/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsClipboard.h"

#include "nsISupportsPrimitives.h"
#include "nsITransferable.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsPrimitiveHelpers.h"
#include "nsString.h"
#include "nsXPCOM.h"

#pragma GCC visibility push(default)
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#pragma GCC visibility pop

using namespace mozilla;

NS_IMPL_ISUPPORTS_INHERITED0(nsClipboard, nsBaseClipboard)

nsClipboard::nsClipboard()
    : nsBaseClipboard(mozilla::dom::ClipboardCapabilities(
          true /* selection */, false /* find */, true /* selection cache */)) {
}

static QClipboard::Mode ToQtMode(nsIClipboard::ClipboardType aType) {
  if (aType == nsIClipboard::kSelectionClipboard) {
    return QClipboard::Selection;
  }
  return QClipboard::Clipboard;
}

NS_IMETHODIMP
nsClipboard::SetNativeClipboardData(nsITransferable* aTransferable,
                                    ClipboardType aWhichClipboard) {
  if (!aTransferable) {
    return NS_ERROR_FAILURE;
  }
  QClipboard* clipboard = QGuiApplication::clipboard();
  if (!clipboard) {
    return NS_ERROR_FAILURE;
  }

  nsTArray<nsCString> flavors;
  nsresult rv = aTransferable->FlavorsTransferableCanExport(flavors);
  if (NS_FAILED(rv)) {
    return rv;
  }

  auto* mime = new QMimeData();
  for (const auto& flavor : flavors) {
    if (flavor.EqualsLiteral(kTextMime)) {
      nsCOMPtr<nsISupports> data;
      if (NS_SUCCEEDED(aTransferable->GetTransferData(flavor.get(),
                                                      getter_AddRefs(data)))) {
        nsCOMPtr<nsISupportsString> str(do_QueryInterface(data));
        if (str) {
          nsAutoString text;
          str->GetData(text);
          mime->setText(QString::fromUtf16(
              reinterpret_cast<const char16_t*>(text.BeginReading()),
              int(text.Length())));
        }
      }
    } else if (flavor.EqualsLiteral(kHTMLMime)) {
      nsCOMPtr<nsISupports> data;
      if (NS_SUCCEEDED(aTransferable->GetTransferData(kHTMLMime,
                                                      getter_AddRefs(data)))) {
        nsCOMPtr<nsISupportsString> str(do_QueryInterface(data));
        if (str) {
          nsAutoString html;
          str->GetData(html);
          mime->setHtml(QString::fromUtf16(
              reinterpret_cast<const char16_t*>(html.BeginReading()),
              int(html.Length())));
        }
      }
    }
  }
  clipboard->setMimeData(mime, ToQtMode(aWhichClipboard));
  return NS_OK;
}

Result<nsCOMPtr<nsISupports>, nsresult> nsClipboard::GetNativeClipboardData(
    const nsACString& aFlavor, ClipboardType aWhichClipboard,
    uint64_t /*aThreshold*/) {
  QClipboard* clipboard = QGuiApplication::clipboard();
  if (!clipboard) {
    return Err(NS_ERROR_FAILURE);
  }
  const QMimeData* mime = clipboard->mimeData(ToQtMode(aWhichClipboard));
  if (!mime) {
    return nsCOMPtr<nsISupports>();
  }

  if (aFlavor.EqualsLiteral(kTextMime) && mime->hasText()) {
    QString t = mime->text();
    nsString text;
    text.Assign(reinterpret_cast<const char16_t*>(t.utf16()), t.size());
    nsCOMPtr<nsISupportsString> data(
        do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID));
    if (!data) {
      return Err(NS_ERROR_OUT_OF_MEMORY);
    }
    data->SetData(text);
    return nsCOMPtr<nsISupports>(data);
  }
  if (aFlavor.EqualsLiteral(kHTMLMime) && mime->hasHtml()) {
    QString t = mime->html();
    nsString text;
    text.Assign(reinterpret_cast<const char16_t*>(t.utf16()), t.size());
    nsCOMPtr<nsISupportsString> data(
        do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID));
    if (!data) {
      return Err(NS_ERROR_OUT_OF_MEMORY);
    }
    data->SetData(text);
    return nsCOMPtr<nsISupports>(data);
  }
  return nsCOMPtr<nsISupports>();
}

nsresult nsClipboard::EmptyNativeClipboardData(ClipboardType aWhichClipboard) {
  if (QClipboard* clipboard = QGuiApplication::clipboard()) {
    clipboard->clear(ToQtMode(aWhichClipboard));
  }
  return NS_OK;
}

Result<int32_t, nsresult> nsClipboard::GetNativeClipboardSequenceNumber(
    ClipboardType) {
  return 0;
}

Result<bool, nsresult> nsClipboard::HasNativeClipboardDataMatchingFlavors(
    const nsTArray<nsCString>& aFlavorList, ClipboardType aWhichClipboard) {
  QClipboard* clipboard = QGuiApplication::clipboard();
  if (!clipboard) {
    return false;
  }
  const QMimeData* mime = clipboard->mimeData(ToQtMode(aWhichClipboard));
  if (!mime) {
    return false;
  }
  for (const auto& flavor : aFlavorList) {
    if (flavor.EqualsLiteral(kTextMime) && mime->hasText()) {
      return true;
    }
    if (flavor.EqualsLiteral(kHTMLMime) && mime->hasHtml()) {
      return true;
    }
  }
  return false;
}
