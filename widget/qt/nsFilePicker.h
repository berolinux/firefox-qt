/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_nsFilePicker_h
#define widget_qt_nsFilePicker_h

#include "nsBaseFilePicker.h"
#include "nsCOMArray.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsIFile.h"

class nsIFilePickerShownCallback;

class nsFilePicker final : public nsBaseFilePicker {
 public:
  nsFilePicker();
  NS_DECL_ISUPPORTS

  NS_IMETHOD GetFilterIndex(int32_t* aFilterIndex) override;
  NS_IMETHOD SetFilterIndex(int32_t aFilterIndex) override;
  NS_IMETHOD GetDefaultString(nsAString& aDefaultString) override;
  NS_IMETHOD SetDefaultString(const nsAString& aDefaultString) override;
  NS_IMETHOD GetDefaultExtension(nsAString& aDefaultExtension) override;
  NS_IMETHOD SetDefaultExtension(const nsAString& aDefaultExtension) override;
  NS_IMETHOD GetFile(nsIFile** aFile) override;
  NS_IMETHOD GetFileURL(nsIURI** aFileURL) override;
  NS_IMETHOD GetFiles(nsISimpleEnumerator** aFiles) override;
  NS_IMETHOD Open(nsIFilePickerShownCallback* aCallback) override;
  NS_IMETHOD AppendFilter(const nsAString& aTitle,
                          const nsAString& aFilter) override;

 private:
  ~nsFilePicker() override;
  void InitNative(nsIWidget* aParent, const nsAString& aTitle) override;

  bool PreferPortal() const;
  void OpenWithQt(nsIFilePickerShownCallback* aCallback);
  bool TryOpenPortal(nsIFilePickerShownCallback* aCallback);

  nsCOMArray<nsIFile> mFiles;
  nsString mTitle;
  nsString mDefault;
  nsString mDefaultExtension;
  int32_t mFilterIndex = 0;
  nsTArray<nsString> mFilters;
  nsTArray<nsString> mFilterNames;
  nsCOMPtr<nsIWidget> mParentWidget;
};

#endif
