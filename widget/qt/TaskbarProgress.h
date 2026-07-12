/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_qt_TaskbarProgress_h
#define mozilla_widget_qt_TaskbarProgress_h

#include "nsIGtkTaskbarProgress.h"
#include "nsCOMPtr.h"
#include "nsString.h"

class nsWindow;

/**
 * Download / taskbar progress for Qt Linux builds.
 *
 * Pure QtDBus (no KF required):
 *  - com.canonical.Unity.LauncherEntry — progress + badge count (Plasma Task Manager)
 *  - org.kde.JobViewServer / JobViewV2 — Plasma notification-area job tracker
 *
 * Optional KF packages (KService, etc.) remain configure-time only and are
 * not linked into this component.
 */
class TaskbarProgress final : public nsIGtkTaskbarProgress {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIGTKTASKBARPROGRESS
  NS_DECL_NSITASKBARPROGRESS

  TaskbarProgress();

 private:
  ~TaskbarProgress();

  void PublishAll();
  void PublishUnityLauncher();
  void EnsureJobView();
  void UpdateJobView();
  void CloseJobView(const nsACString& aErrorMessage);
  void PublishXAppProperty(unsigned long aPercent);

  static nsCString DesktopFileUri();
  static nsCString AppName();
  static nsCString AppIconName();

  unsigned long mCurrentProgress = 0;
  unsigned long mActiveCount = 0;
  uint64_t mCurrentBytes = 0;
  uint64_t mMaxBytes = 0;
  nsTaskbarProgressState mCurrentState = STATE_NO_PROGRESS;
  RefPtr<nsWindow> mPrimaryWindow;

  // org.kde.JobViewV2 object path on the session bus (empty when inactive).
  nsCString mJobViewPath;
  nsCString mJobViewService;
  bool mJobViewTried = false;
};

#endif
