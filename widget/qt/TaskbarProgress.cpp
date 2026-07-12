/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TaskbarProgress.h"

#include "mozilla/Logging.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/XREAppData.h"
#include "nsWindow.h"
#include "WidgetUtils.h"
#include "nsPIDOMWindow.h"
#include "nsString.h"
#include "prenv.h"

#include <algorithm>
#include <cstring>

#pragma GCC visibility push(default)
#include <QGuiApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QVariantMap>
#pragma GCC visibility pop

using mozilla::LogLevel;
static mozilla::LazyLogModule gQtTaskbarProgressLog("nsIQtTaskbarProgress");

extern const mozilla::XREAppData* gAppData;

namespace {

// Unity / Plasma launcher protocol.
constexpr int kJobCapCancel = 0x0001;

bool PrefLauncherCount() {
  return mozilla::StaticPrefs::widget_qt_downloads_launcher_count_enabled();
}

bool PrefJobView() {
  return mozilla::StaticPrefs::widget_qt_downloads_jobview_enabled();
}

bool IsPlasmaSession() {
  if (const char* d = PR_GetEnv("XDG_CURRENT_DESKTOP")) {
    if (strstr(d, "KDE") || strstr(d, "Plasma")) {
      return true;
    }
  }
  if (const char* s = PR_GetEnv("DESKTOP_SESSION")) {
    if (strstr(s, "plasma") || strstr(s, "kde")) {
      return true;
    }
  }
  return PR_GetEnv("KDE_FULL_SESSION") != nullptr;
}

}  // namespace

NS_IMPL_ISUPPORTS(TaskbarProgress, nsIGtkTaskbarProgress, nsITaskbarProgress)

TaskbarProgress::TaskbarProgress() {
  MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Info,
          ("%p TaskbarProgress()", this));
}

TaskbarProgress::~TaskbarProgress() {
  CloseJobView(""_ns);
  mCurrentProgress = 0;
  mActiveCount = 0;
  mCurrentBytes = 0;
  mMaxBytes = 0;
  mCurrentState = STATE_NO_PROGRESS;
  PublishUnityLauncher();
  MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Info,
          ("%p ~TaskbarProgress()", this));
}

/* static */
nsCString TaskbarProgress::DesktopFileUri() {
  nsCString id;
  if (const char* env = PR_GetEnv("MOZ_DESKTOP_FILE_NAME")) {
    id.Assign(env);
  } else if (gAppData && gAppData->remotingName) {
    id.Assign(gAppData->remotingName);
  } else {
    id.AssignLiteral("firefox");
  }
  if (!StringEndsWith(id, ".desktop"_ns)) {
    id.AppendLiteral(".desktop");
  }
  nsCString uri;
  uri.AssignLiteral("application://");
  uri.Append(id);
  return uri;
}

/* static */
nsCString TaskbarProgress::AppName() {
  if (gAppData && gAppData->name) {
    return nsCString(gAppData->name);
  }
  return "Firefox"_ns;
}

/* static */
nsCString TaskbarProgress::AppIconName() {
  if (const char* env = PR_GetEnv("MOZ_APP_ICON")) {
    return nsCString(env);
  }
  if (gAppData && gAppData->remotingName) {
    return nsCString(gAppData->remotingName);
  }
  return "firefox"_ns;
}

void TaskbarProgress::PublishUnityLauncher() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    return;
  }

  const bool noProgress = mCurrentState == STATE_NO_PROGRESS;
  const bool indeterminate = mCurrentState == STATE_INDETERMINATE;
  const bool urgent = mCurrentState == STATE_ERROR;
  const bool countEnabled = PrefLauncherCount();
  const bool showCount = countEnabled && mActiveCount > 0 && !noProgress;
  const bool showProgress = !noProgress;

  QVariantMap props;
  if (noProgress) {
    props.insert(QStringLiteral("progress-visible"), false);
    props.insert(QStringLiteral("progress"), 0.0);
    props.insert(QStringLiteral("urgent"), false);
    props.insert(QStringLiteral("count-visible"), false);
    props.insert(QStringLiteral("count"), qlonglong(0));
  } else if (indeterminate) {
    props.insert(QStringLiteral("progress-visible"), true);
    props.insert(QStringLiteral("progress"), 0.0);
    props.insert(QStringLiteral("urgent"), true);
    props.insert(QStringLiteral("count-visible"), showCount);
    props.insert(QStringLiteral("count"), qlonglong(mActiveCount));
  } else {
    const double frac = std::clamp(mCurrentProgress / 100.0, 0.0, 1.0);
    props.insert(QStringLiteral("progress-visible"), showProgress);
    props.insert(QStringLiteral("progress"), frac);
    props.insert(QStringLiteral("urgent"), urgent);
    props.insert(QStringLiteral("count-visible"), showCount);
    props.insert(QStringLiteral("count"), qlonglong(mActiveCount));
  }

  QDBusMessage signal = QDBusMessage::createSignal(
      QStringLiteral("/"), QStringLiteral("com.canonical.Unity.LauncherEntry"),
      QStringLiteral("Update"));
  signal << QString::fromUtf8(DesktopFileUri().get());
  signal << props;
  if (!bus.send(signal)) {
    MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Debug,
            ("Unity LauncherEntry.Update send failed"));
  } else {
    MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Debug,
            ("Unity Update uri=%s progress=%lu count=%lu state=%ld",
             DesktopFileUri().get(), mCurrentProgress, mActiveCount,
             long(mCurrentState)));
  }
}

void TaskbarProgress::EnsureJobView() {
  if (!PrefJobView() || !IsPlasmaSession()) {
    return;
  }
  if (!mJobViewPath.IsEmpty() || mJobViewTried) {
    return;
  }
  mJobViewTried = true;

  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    return;
  }

  auto tryRequest = [&](const char* service, const char* path) -> bool {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), QString::fromLatin1(path),
        QStringLiteral("org.kde.JobViewServer"),
        QStringLiteral("requestView"));
    msg << QString::fromUtf8(AppName().get())
        << QString::fromUtf8(AppIconName().get()) << int(kJobCapCancel);
    QDBusMessage reply = bus.call(msg, QDBus::Block, 2000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
      MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Debug,
              ("JobView requestView %s failed: %s", service,
               qPrintable(reply.errorMessage())));
      return false;
    }
    if (reply.arguments().isEmpty()) {
      return false;
    }
    const QVariant v = reply.arguments().at(0);
    QString objPath;
    if (v.canConvert<QDBusObjectPath>()) {
      objPath = v.value<QDBusObjectPath>().path();
    } else {
      objPath = v.toString();
    }
    if (objPath.isEmpty()) {
      return false;
    }
    mJobViewPath.Assign(objPath.toUtf8().constData());
    mJobViewService.Assign(service);
    MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Info,
            ("JobView path %s via %s", mJobViewPath.get(), service));

    // Initial description.
    QDBusMessage info = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), objPath,
        QStringLiteral("org.kde.JobViewV2"), QStringLiteral("setInfoMessage"));
    info << QStringLiteral("Downloading");
    bus.call(info, QDBus::NoBlock);
    return true;
  };

  // Plasma uses kuiserver historically; some builds export JobViewServer name.
  if (!tryRequest("org.kde.kuiserver", "/JobViewServer")) {
    (void)tryRequest("org.kde.JobViewServer", "/JobViewServer");
  }
}

void TaskbarProgress::UpdateJobView() {
  if (mJobViewPath.IsEmpty() || mJobViewService.IsEmpty()) {
    return;
  }
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    return;
  }

  auto callNoReply = [&](const char* method, const QVariantList& args) {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromUtf8(mJobViewService.get()),
        QString::fromUtf8(mJobViewPath.get()),
        QStringLiteral("org.kde.JobViewV2"), QString::fromLatin1(method));
    for (const QVariant& a : args) {
      msg << a;
    }
    bus.send(msg);
  };

  if (mCurrentState == STATE_NO_PROGRESS) {
    return;
  }

  if (mCurrentState == STATE_INDETERMINATE) {
    callNoReply("setPercent", {QVariant::fromValue(uint(0))});
  } else {
    callNoReply("setPercent",
                {QVariant::fromValue(uint(mCurrentProgress))});
  }

  if (mMaxBytes > 0) {
    callNoReply("setTotalAmount",
                {QVariant::fromValue(quint64(mMaxBytes)),
                 QStringLiteral("bytes")});
    callNoReply("setProcessedAmount",
                {QVariant::fromValue(quint64(mCurrentBytes)),
                 QStringLiteral("bytes")});
  }

  nsAutoCString info;
  if (mActiveCount > 1) {
    info.AppendInt(int(mActiveCount));
    info.AppendLiteral(" downloads");
  } else {
    info.AssignLiteral("Downloading");
  }
  callNoReply("setInfoMessage", {QString::fromUtf8(info.get())});
}

void TaskbarProgress::CloseJobView(const nsACString& aErrorMessage) {
  if (mJobViewPath.IsEmpty()) {
    mJobViewTried = false;
    mJobViewService.Truncate();
    return;
  }
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (bus.isConnected() && !mJobViewService.IsEmpty()) {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromUtf8(mJobViewService.get()),
        QString::fromUtf8(mJobViewPath.get()),
        QStringLiteral("org.kde.JobViewV2"), QStringLiteral("terminate"));
    msg << QString::fromUtf8(PromiseFlatCString(aErrorMessage).get());
    bus.send(msg);
  }
  MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Debug,
          ("JobView closed path=%s", mJobViewPath.get()));
  mJobViewPath.Truncate();
  mJobViewService.Truncate();
  mJobViewTried = false;
}

void TaskbarProgress::PublishXAppProperty(unsigned long) {
  // Plasma uses Unity LauncherEntry + JobView over D-Bus (X11 and Wayland).
}

void TaskbarProgress::PublishAll() {
  PublishUnityLauncher();

  if (mCurrentState == STATE_NO_PROGRESS) {
    CloseJobView(""_ns);
    return;
  }

  EnsureJobView();
  UpdateJobView();
}

NS_IMETHODIMP
TaskbarProgress::SetProgressState(nsTaskbarProgressState aState,
                                  uint64_t aCurrentValue, uint64_t aMaxValue) {
  NS_ENSURE_ARG_RANGE(aState, 0, STATE_PAUSED);

  if (aState == STATE_NO_PROGRESS || aState == STATE_INDETERMINATE) {
    NS_ENSURE_TRUE(aCurrentValue == 0, NS_ERROR_INVALID_ARG);
    NS_ENSURE_TRUE(aMaxValue == 0, NS_ERROR_INVALID_ARG);
  }
  NS_ENSURE_TRUE(aCurrentValue <= aMaxValue, NS_ERROR_ILLEGAL_VALUE);

  unsigned long progress = 0;
  if (aState != STATE_INDETERMINATE && aState != STATE_NO_PROGRESS &&
      aMaxValue != 0) {
    progress = static_cast<unsigned long>(
        (static_cast<double>(aCurrentValue) / aMaxValue) * 100.0);
  }

  if (progress == mCurrentProgress && aState == mCurrentState &&
      aCurrentValue == mCurrentBytes && aMaxValue == mMaxBytes) {
    return NS_OK;
  }
  mCurrentProgress = progress;
  mCurrentState = aState;
  mCurrentBytes = aCurrentValue;
  mMaxBytes = aMaxValue;

  MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Debug,
          ("SetProgressState state=%ld progress=%lu bytes=%llu/%llu",
           long(aState), progress, (unsigned long long)aCurrentValue,
           (unsigned long long)aMaxValue));

  PublishAll();
  return NS_OK;
}

NS_IMETHODIMP
TaskbarProgress::SetActiveDownloadCount(uint32_t aCount) {
  if (aCount == mActiveCount) {
    return NS_OK;
  }
  mActiveCount = aCount;
  MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Debug,
          ("SetActiveDownloadCount %u", aCount));
  // Refresh badge even when progress percent is unchanged.
  PublishUnityLauncher();
  if (!mJobViewPath.IsEmpty()) {
    UpdateJobView();
  }
  return NS_OK;
}

NS_IMETHODIMP
TaskbarProgress::SetPrimaryWindow(mozIDOMWindowProxy* aWindow) {
  NS_ENSURE_TRUE(aWindow, NS_ERROR_ILLEGAL_VALUE);

  auto* parent = nsPIDOMWindowOuter::From(aWindow);
  RefPtr<nsIWidget> widget =
      mozilla::widget::WidgetUtils::DOMWindowToWidget(parent);
  NS_ENSURE_TRUE(widget, NS_ERROR_FAILURE);

  mPrimaryWindow = nsWindow::FromWidget(widget);
  mCurrentProgress = 0;
  mCurrentState = STATE_NO_PROGRESS;
  mCurrentBytes = 0;
  mMaxBytes = 0;

  MOZ_LOG(gQtTaskbarProgressLog, LogLevel::Debug,
          ("SetPrimaryWindow window: %p", mPrimaryWindow.get()));
  return NS_OK;
}
