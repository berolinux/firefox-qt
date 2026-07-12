/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsPrintDialogServiceQt.h"

#include "mozilla/dom/Promise.h"
#include "mozilla/ErrorResult.h"
#include "nsIPrintSettings.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "xpcpublic.h"

#pragma GCC visibility push(default)
#include <QAbstractPrintDialog>
#include <QPageLayout>
#include <QPageSetupDialog>
#include <QPageSize>
#include <QPrintDialog>
#include <QPrinter>
#include <QWidget>
#pragma GCC visibility pop

using namespace mozilla;
using mozilla::dom::Promise;

NS_IMPL_ISUPPORTS(nsPrintDialogServiceQt, nsIPrintDialogService)

NS_IMETHODIMP
nsPrintDialogServiceQt::Init() { return NS_OK; }

static void SettingsToPrinter(nsIPrintSettings* aSettings, QPrinter& aPrinter) {
  nsString printerName;
  if (NS_SUCCEEDED(aSettings->GetPrinterName(printerName)) &&
      !printerName.IsEmpty()) {
    aPrinter.setPrinterName(
        QString::fromUtf16(reinterpret_cast<const char16_t*>(
                               printerName.BeginReading()),
                           int(printerName.Length())));
  }

  int32_t copies = 1;
  if (NS_SUCCEEDED(aSettings->GetNumCopies(&copies)) && copies > 0) {
    aPrinter.setCopyCount(copies);
  }

  aPrinter.setColorMode(aSettings->GetPrintInColor() ? QPrinter::Color
                                                     : QPrinter::GrayScale);

  switch (aSettings->GetDuplex()) {
    case nsIPrintSettings::kDuplexFlipOnLongEdge:
      aPrinter.setDuplex(QPrinter::DuplexLongSide);
      break;
    case nsIPrintSettings::kDuplexFlipOnShortEdge:
      aPrinter.setDuplex(QPrinter::DuplexShortSide);
      break;
    default:
      aPrinter.setDuplex(QPrinter::DuplexNone);
      break;
  }

  int32_t orientation = nsIPrintSettings::kPortraitOrientation;
  aSettings->GetOrientation(&orientation);
  QPageLayout layout = aPrinter.pageLayout();
  layout.setOrientation(orientation == nsIPrintSettings::kLandscapeOrientation
                            ? QPageLayout::Landscape
                            : QPageLayout::Portrait);
  aPrinter.setPageLayout(layout);

  nsString toFile;
  if (NS_SUCCEEDED(aSettings->GetToFileName(toFile)) && !toFile.IsEmpty() &&
      aSettings->GetOutputDestination() ==
          nsIPrintSettings::kOutputDestinationFile) {
    aPrinter.setOutputFormat(QPrinter::PdfFormat);
    aPrinter.setOutputFileName(QString::fromUtf16(
        reinterpret_cast<const char16_t*>(toFile.BeginReading()),
        int(toFile.Length())));
  }
}

static void PrinterToSettings(const QPrinter& aPrinter,
                              nsIPrintSettings* aSettings) {
  const QString name = aPrinter.printerName();
  if (!name.isEmpty()) {
    nsString printerName;
    printerName.Assign(reinterpret_cast<const char16_t*>(name.utf16()),
                       name.size());
    aSettings->SetPrinterName(printerName);
  }

  aSettings->SetNumCopies(aPrinter.copyCount());
  aSettings->SetPrintInColor(aPrinter.colorMode() == QPrinter::Color);

  switch (aPrinter.duplex()) {
    case QPrinter::DuplexLongSide:
    case QPrinter::DuplexAuto:
      aSettings->SetDuplex(nsIPrintSettings::kDuplexFlipOnLongEdge);
      break;
    case QPrinter::DuplexShortSide:
      aSettings->SetDuplex(nsIPrintSettings::kDuplexFlipOnShortEdge);
      break;
    default:
      aSettings->SetDuplex(nsIPrintSettings::kDuplexNone);
      break;
  }

  const QPageLayout layout = aPrinter.pageLayout();
  aSettings->SetOrientation(
      layout.orientation() == QPageLayout::Landscape
          ? nsIPrintSettings::kLandscapeOrientation
          : nsIPrintSettings::kPortraitOrientation);

  // We always render PDF for CUPS; native vs PDF is only about destination.
  aSettings->SetOutputFormat(nsIPrintSettings::kOutputFormatPDF);

  if (aPrinter.outputFormat() == QPrinter::PdfFormat &&
      !aPrinter.outputFileName().isEmpty()) {
    const QString path = aPrinter.outputFileName();
    nsString fileName;
    fileName.Assign(reinterpret_cast<const char16_t*>(path.utf16()),
                    path.size());
    aSettings->SetToFileName(fileName);
    aSettings->SetOutputDestination(nsIPrintSettings::kOutputDestinationFile);
  } else {
    aSettings->SetOutputDestination(
        nsIPrintSettings::kOutputDestinationPrinter);
  }

  const QPageSize pageSize = layout.pageSize();
  if (pageSize.isValid()) {
    const QString key = pageSize.key();
    if (!key.isEmpty()) {
      nsString paperId;
      paperId.Assign(reinterpret_cast<const char16_t*>(key.utf16()),
                     key.size());
      aSettings->SetPaperId(paperId);
    }
    // nsIPrintSettings paper dimensions are in the unit of paperSizeUnit
    // (default inches).
    aSettings->SetPaperSizeUnit(nsIPrintSettings::kPaperSizeInches);
    const QSizeF sizeIn = pageSize.size(QPageSize::Inch);
    aSettings->SetPaperWidth(sizeIn.width());
    aSettings->SetPaperHeight(sizeIn.height());
  }
}

NS_IMETHODIMP
nsPrintDialogServiceQt::ShowPrintDialog(mozIDOMWindowProxy* aParent,
                                        bool /*aHaveSelection*/,
                                        nsIPrintSettings* aSettings,
                                        JSContext* aCx, Promise** aPromise) {
  MOZ_ASSERT(NS_IsMainThread());
  NS_ENSURE_ARG(aSettings);
  NS_ENSURE_ARG(aCx);
  NS_ENSURE_ARG(aPromise);
  (void)aParent;

  ErrorResult rvErr;
  nsCOMPtr<nsIGlobalObject> global = xpc::CurrentNativeGlobal(aCx);
  RefPtr<Promise> promise = Promise::Create(global, rvErr);
  if (NS_WARN_IF(rvErr.Failed())) {
    return rvErr.StealNSResult();
  }

  QPrinter printer(QPrinter::HighResolution);
  SettingsToPrinter(aSettings, printer);

  QPrintDialog dialog(&printer);
  dialog.setOption(QAbstractPrintDialog::PrintToFile, true);
  dialog.setOption(QAbstractPrintDialog::PrintCollateCopies, true);
  dialog.setOption(QAbstractPrintDialog::PrintShowPageSize, true);
  dialog.setOption(QAbstractPrintDialog::PrintPageRange, true);
  dialog.setWindowModality(Qt::ApplicationModal);

  nsresult rv = NS_OK;
  if (dialog.exec() != QDialog::Accepted) {
    rv = NS_ERROR_ABORT;
  } else {
    PrinterToSettings(printer, aSettings);
  }

  if (NS_SUCCEEDED(rv)) {
    promise->MaybeResolveWithUndefined();
  } else {
    promise->MaybeReject(rv);
  }
  promise.forget(aPromise);
  return NS_OK;
}

NS_IMETHODIMP
nsPrintDialogServiceQt::ShowPageSetupDialog(mozIDOMWindowProxy* aParent,
                                            nsIPrintSettings* aSettings,
                                            JSContext* aCx,
                                            Promise** aPromise) {
  MOZ_ASSERT(NS_IsMainThread());
  NS_ENSURE_ARG(aSettings);
  NS_ENSURE_ARG(aCx);
  NS_ENSURE_ARG(aPromise);
  (void)aParent;

  ErrorResult rvErr;
  nsCOMPtr<nsIGlobalObject> global = xpc::CurrentNativeGlobal(aCx);
  RefPtr<Promise> promise = Promise::Create(global, rvErr);
  if (NS_WARN_IF(rvErr.Failed())) {
    return rvErr.StealNSResult();
  }

  QPrinter printer(QPrinter::HighResolution);
  SettingsToPrinter(aSettings, printer);

  QPageSetupDialog dialog(&printer);
  dialog.setWindowModality(Qt::ApplicationModal);

  nsresult rv = NS_OK;
  if (dialog.exec() != QDialog::Accepted) {
    rv = NS_ERROR_ABORT;
  } else {
    PrinterToSettings(printer, aSettings);
  }

  if (NS_SUCCEEDED(rv)) {
    promise->MaybeResolveWithUndefined();
  } else {
    promise->MaybeReject(rv);
  }
  promise.forget(aPromise);
  return NS_OK;
}
