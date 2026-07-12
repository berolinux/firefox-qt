/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsFilePicker.h"

#include "nsArrayEnumerator.h"
#include "nsEnumeratorUtils.h"
#include "nsIFileURL.h"
#include "nsNetUtil.h"
#include "nsIWidget.h"
#include "mozilla/StaticPrefs_widget.h"
#include "prenv.h"

#ifdef MOZ_ENABLE_DBUS
#  include "PortalFileChooser.h"
#  include "PortalUtils.h"
#  include "mozilla/Logging.h"
#endif

#pragma GCC visibility push(default)
#include <QFileDialog>
#include <QStringList>
#pragma GCC visibility pop

using namespace mozilla;

#ifdef MOZ_ENABLE_DBUS
static LazyLogModule gQtFilePickerLog("QtFilePicker");
#  define LOG(...) \
    MOZ_LOG(gQtFilePickerLog, LogLevel::Debug, (__VA_ARGS__))
#endif

NS_IMPL_ISUPPORTS(nsFilePicker, nsIFilePicker)

nsFilePicker::nsFilePicker() = default;

nsFilePicker::~nsFilePicker() = default;

void nsFilePicker::InitNative(nsIWidget* aParent, const nsAString& aTitle) {
  mParentWidget = aParent;
  mTitle = aTitle;
}

NS_IMETHODIMP
nsFilePicker::GetFilterIndex(int32_t* aFilterIndex) {
  *aFilterIndex = mFilterIndex;
  return NS_OK;
}

NS_IMETHODIMP
nsFilePicker::SetFilterIndex(int32_t aFilterIndex) {
  mFilterIndex = aFilterIndex;
  return NS_OK;
}

NS_IMETHODIMP
nsFilePicker::GetDefaultString(nsAString& aDefaultString) {
  aDefaultString = mDefault;
  return NS_OK;
}

NS_IMETHODIMP
nsFilePicker::SetDefaultString(const nsAString& aDefaultString) {
  mDefault = aDefaultString;
  return NS_OK;
}

NS_IMETHODIMP
nsFilePicker::GetDefaultExtension(nsAString& aDefaultExtension) {
  aDefaultExtension = mDefaultExtension;
  return NS_OK;
}

NS_IMETHODIMP
nsFilePicker::SetDefaultExtension(const nsAString& aDefaultExtension) {
  mDefaultExtension = aDefaultExtension;
  return NS_OK;
}

NS_IMETHODIMP
nsFilePicker::GetFile(nsIFile** aFile) {
  *aFile = nullptr;
  if (mFiles.Count() == 0) {
    return NS_OK;
  }
  NS_ADDREF(*aFile = mFiles[0]);
  return NS_OK;
}

NS_IMETHODIMP
nsFilePicker::GetFileURL(nsIURI** aFileURL) {
  *aFileURL = nullptr;
  nsCOMPtr<nsIFile> file;
  nsresult rv = GetFile(getter_AddRefs(file));
  if (!file) {
    return rv;
  }
  return NS_NewFileURI(aFileURL, file);
}

NS_IMETHODIMP
nsFilePicker::GetFiles(nsISimpleEnumerator** aFiles) {
  return NS_NewArrayEnumerator(aFiles, mFiles, NS_GET_IID(nsIFile));
}

NS_IMETHODIMP
nsFilePicker::AppendFilter(const nsAString& aTitle, const nsAString& aFilter) {
  mFilterNames.AppendElement(aTitle);
  mFilters.AppendElement(aFilter);
  return NS_OK;
}

bool nsFilePicker::PreferPortal() const {
#ifdef MOZ_ENABLE_DBUS
  return widget::ShouldUsePortal(widget::PortalKind::FilePicker);
#else
  return false;
#endif
}

void nsFilePicker::OpenWithQt(nsIFilePickerShownCallback* aCallback) {
  mFiles.Clear();
  QString title = QString::fromUtf16(
      reinterpret_cast<const char16_t*>(mTitle.BeginReading()),
      int(mTitle.Length()));

  QStringList selected;
  nsIFilePicker::ResultCode result = nsIFilePicker::returnCancel;

  if (mMode == nsIFilePicker::modeGetFolder) {
    QString dir = QFileDialog::getExistingDirectory(nullptr, title);
    if (!dir.isEmpty()) {
      selected << dir;
      result = nsIFilePicker::returnOK;
    }
  } else if (mMode == nsIFilePicker::modeSave) {
    QString file = QFileDialog::getSaveFileName(nullptr, title);
    if (!file.isEmpty()) {
      selected << file;
      result = nsIFilePicker::returnOK;
    }
  } else if (mMode == nsIFilePicker::modeOpenMultiple) {
    selected = QFileDialog::getOpenFileNames(nullptr, title);
    if (!selected.isEmpty()) {
      result = nsIFilePicker::returnOK;
    }
  } else {
    QString file = QFileDialog::getOpenFileName(nullptr, title);
    if (!file.isEmpty()) {
      selected << file;
      result = nsIFilePicker::returnOK;
    }
  }

  for (const QString& path : selected) {
    nsCOMPtr<nsIFile> file;
    if (NS_SUCCEEDED(NS_NewNativeLocalFile(
            nsDependentCString(path.toUtf8().constData()),
            getter_AddRefs(file)))) {
      mFiles.AppendObject(file);
    }
  }

  if (aCallback) {
    aCallback->Done(result);
  }
}

bool nsFilePicker::TryOpenPortal(nsIFilePickerShownCallback* aCallback) {
#ifdef MOZ_ENABLE_DBUS
  auto* chooser = new widget::PortalFileChooser(
      [self = RefPtr{this}, cb = nsCOMPtr{aCallback}](
          int32_t aResultCode, const QStringList& aPaths) {
        self->mFiles.Clear();
        if (aResultCode == int32_t(nsIFilePicker::returnOK)) {
          for (const QString& path : aPaths) {
            nsCOMPtr<nsIFile> file;
            if (NS_SUCCEEDED(NS_NewNativeLocalFile(
                    nsDependentCString(path.toUtf8().constData()),
                    getter_AddRefs(file)))) {
              self->mFiles.AppendObject(file);
            }
          }
          if (self->mFiles.Count() == 0) {
            aResultCode = int32_t(nsIFilePicker::returnCancel);
          }
        }
        if (aResultCode != int32_t(nsIFilePicker::returnOK)) {
          // Portal failed or cancelled after start — fall back to QFileDialog
          // only on hard failure (cancel is cancel).
          LOG("portal finished result=%d paths=%d", aResultCode,
              int(aPaths.size()));
        }
        if (cb) {
          cb->Done(nsIFilePicker::ResultCode(aResultCode));
        }
      });

  if (chooser->Start(mMode, mTitle, mDefault, mFilters, mFilterNames)) {
    return true;
  }
  delete chooser;
  LOG("portal Start failed; falling back to QFileDialog");
#endif
  return false;
}

NS_IMETHODIMP
nsFilePicker::Open(nsIFilePickerShownCallback* aCallback) {
  mFiles.Clear();
  if (PreferPortal() && TryOpenPortal(aCallback)) {
    return NS_OK;
  }
  OpenWithQt(aCallback);
  return NS_OK;
}
