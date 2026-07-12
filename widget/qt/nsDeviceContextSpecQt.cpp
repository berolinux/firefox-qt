/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsDeviceContextSpecQt.h"

#include "mozilla/gfx/PrintPromise.h"
#include "mozilla/gfx/PrintTargetPDF.h"
#include "mozilla/Logging.h"

#include "nsAnonymousTemporaryFile.h"
#include "nsComponentManagerUtils.h"
#include "nsCUPSShim.h"
#include "nsIFile.h"
#include "nsIFileStreams.h"
#include "nsIPrintSettings.h"
#include "nsPrinterCUPS.h"
#include "nsPrintfCString.h"
#include "nsReadableUtils.h"
#include "nsString.h"

#include <sys/stat.h>
#include <sys/types.h>

using namespace mozilla;
using namespace mozilla::gfx;

static LazyLogModule gQtPrintLog("nsDeviceContextSpecQt");
#define QTPRINT_LOG(...) \
  MOZ_LOG(gQtPrintLog, mozilla::LogLevel::Debug, (__VA_ARGS__))

NS_IMPL_ISUPPORTS(nsDeviceContextSpecQt, nsIDeviceContextSpec)

nsDeviceContextSpecQt::~nsDeviceContextSpecQt() {
  if (mSpoolFile) {
    mSpoolFile->Remove(false);
  }
}

already_AddRefed<PrintTarget> nsDeviceContextSpecQt::MakePrintTarget() {
  double width = 0;
  double height = 0;
  mPrintSettings->GetEffectiveSheetSize(&width, &height);

  // convert twips to points
  width /= TWIPS_PER_POINT_FLOAT;
  height /= TWIPS_PER_POINT_FLOAT;

  MOZ_ASSERT(!mSpoolFile);

  auto stream = [&]() -> nsCOMPtr<nsIOutputStream> {
    if (mPrintSettings->GetOutputDestination() ==
        nsIPrintSettings::kOutputDestinationStream) {
      nsCOMPtr<nsIOutputStream> out;
      mPrintSettings->GetOutputStream(getter_AddRefs(out));
      return out;
    }

    // Spool PDF for print-to-file or print-to-printer.
    if (NS_FAILED(NS_OpenAnonymousTemporaryNsIFile(getter_AddRefs(mSpoolFile)))) {
      return nullptr;
    }
    mSpoolFile->SetPermissions(0600);
    nsCOMPtr<nsIFileOutputStream> s =
        do_CreateInstance("@mozilla.org/network/file-output-stream;1");
    if (NS_FAILED(s->Init(mSpoolFile, -1, -1, 0))) {
      mSpoolFile = nullptr;
      return nullptr;
    }
    return s;
  }();

  if (!stream) {
    return nullptr;
  }
  return PrintTargetPDF::CreateOrNull(stream, IntSize::Ceil(width, height));
}

NS_IMETHODIMP
nsDeviceContextSpecQt::Init(nsIPrintSettings* aPS, bool /*aIsPrintPreview*/) {
  mPrintSettings = aPS;
  return aPS ? NS_OK : NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsDeviceContextSpecQt::BeginDocument(const nsAString& aTitle,
                                     const nsAString& /*aPrintToFileName*/,
                                     uint64_t /*aBrowsingContextId*/,
                                     int32_t /*aStartPage*/,
                                     int32_t /*aEndPage*/) {
  PrintTarget::AdjustPrintJobNameForIPP(aTitle, mTitle);
  return NS_OK;
}

nsresult nsDeviceContextSpecQt::SubmitSpoolToPrinter() {
  if (!mSpoolFile) {
    return NS_ERROR_FAILURE;
  }

  nsString printerName;
  nsresult rv = mPrintSettings->GetPrinterName(printerName);
  if (NS_FAILED(rv) || printerName.IsEmpty()) {
    QTPRINT_LOG("No printer name set");
    return NS_ERROR_FAILURE;
  }

  nsCUPSShim cups;
  if (!cups.InitOkay()) {
    QTPRINT_LOG("CUPS not available");
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsAutoCString spoolPath;
  rv = mSpoolFile->GetNativePath(spoolPath);
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ConvertUTF16toUTF8 printer(printerName);

  int numOptions = 0;
  cups_option_t* options = nullptr;

  int32_t copies = 1;
  if (NS_SUCCEEDED(mPrintSettings->GetNumCopies(&copies)) && copies > 1) {
    numOptions = cups.cupsAddOption(CUPS_COPIES,
                                    nsPrintfCString("%d", copies).get(),
                                    numOptions, &options);
  }

  // Duplex: Gecko's long-edge/short-edge map to CUPS portrait/landscape sides.
  const int32_t duplex = mPrintSettings->GetDuplex();
  const char* sides = CUPS_SIDES_ONE_SIDED;
  if (duplex == nsIPrintSettings::kDuplexFlipOnLongEdge) {
    sides = CUPS_SIDES_TWO_SIDED_PORTRAIT;
  } else if (duplex == nsIPrintSettings::kDuplexFlipOnShortEdge) {
    sides = CUPS_SIDES_TWO_SIDED_LANDSCAPE;
  }
  numOptions =
      cups.cupsAddOption(CUPS_SIDES, sides, numOptions, &options);

  if (mPrintSettings->GetPrintInColor()) {
    numOptions = cups.cupsAddOption(CUPS_PRINT_COLOR_MODE,
                                    CUPS_PRINT_COLOR_MODE_COLOR, numOptions,
                                    &options);
  } else {
    numOptions = cups.cupsAddOption(CUPS_PRINT_COLOR_MODE,
                                    CUPS_PRINT_COLOR_MODE_MONOCHROME,
                                    numOptions, &options);
#define APPLY_MONO(key_, value_) \
  numOptions = cups.cupsAddOption(key_, value_, numOptions, &options);
    CUPS_EACH_MONOCHROME_PRINTER_SETTING(APPLY_MONO)
#undef APPLY_MONO
    auto applyExtra = [&](const nsACString& aKey, const nsACString& aVal) {
      numOptions = cups.cupsAddOption(PromiseFlatCString(aKey).get(),
                                      PromiseFlatCString(aVal).get(),
                                      numOptions, &options);
    };
    nsPrinterCUPS::ForEachExtraMonochromeSetting(applyExtra);
  }

  nsString paperId;
  if (NS_SUCCEEDED(mPrintSettings->GetPaperId(paperId)) && !paperId.IsEmpty()) {
    NS_ConvertUTF16toUTF8 media(paperId);
    numOptions =
        cups.cupsAddOption(CUPS_MEDIA, media.get(), numOptions, &options);
  }

  const char* title =
      mTitle.IsEmpty() ? "Mozilla Firefox" : mTitle.get();
  const int jobId = cups.cupsPrintFile(printer.get(), spoolPath.get(), title,
                                       numOptions, options);
  cups.cupsFreeOptions(numOptions, options);

  if (jobId <= 0) {
    QTPRINT_LOG("cupsPrintFile failed for printer '%s' (jobId=%d)",
                printer.get(), jobId);
    return NS_ERROR_FAILURE;
  }

  QTPRINT_LOG("Submitted job %d to printer '%s'", jobId, printer.get());
  mSpoolFile->Remove(false);
  mSpoolFile = nullptr;
  return NS_OK;
}

RefPtr<PrintEndDocumentPromise> nsDeviceContextSpecQt::EndDocument() {
  switch (mPrintSettings->GetOutputDestination()) {
    case nsIPrintSettings::kOutputDestinationPrinter: {
      return nsIDeviceContextSpec::EndDocumentAsync(
          __func__, [self = RefPtr{this}]() { return self->SubmitSpoolToPrinter(); });
    }

    case nsIPrintSettings::kOutputDestinationFile: {
      if (!mSpoolFile) {
        return PrintEndDocumentPromise::CreateAndReject(NS_ERROR_FAILURE,
                                                        __func__);
      }
      nsString targetPath;
      mPrintSettings->GetToFileName(targetPath);
      nsCOMPtr<nsIFile> destFile;
      nsresult rv = NS_NewLocalFile(targetPath, getter_AddRefs(destFile));
      if (NS_FAILED(rv)) {
        return PrintEndDocumentPromise::CreateAndReject(rv, __func__);
      }

      return nsIDeviceContextSpec::EndDocumentAsync(
          __func__,
          [destFile = std::move(destFile),
           spoolFile = std::move(mSpoolFile)]() -> nsresult {
            nsAutoString destLeafName;
            nsresult rv = destFile->GetLeafName(destLeafName);
            NS_ENSURE_SUCCESS(rv, rv);

            nsCOMPtr<nsIFile> destDir;
            rv = destFile->GetParent(getter_AddRefs(destDir));
            NS_ENSURE_SUCCESS(rv, rv);

            rv = spoolFile->MoveTo(destDir, destLeafName);
            NS_ENSURE_SUCCESS(rv, rv);

            mode_t mask = umask(0);
            umask(mask);
            destFile->SetPermissions(0666 & ~(mask));
            return NS_OK;
          });
    }

    case nsIPrintSettings::kOutputDestinationStream:
      // Handled in MakePrintTarget; nothing left to do.
      MOZ_ASSERT(!mSpoolFile);
      break;
  }
  return PrintEndDocumentPromise::CreateAndResolve(true, __func__);
}
