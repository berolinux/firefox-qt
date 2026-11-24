/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAppShell.h"

#include "HeadlessScreenHelper.h"
#include "QtAppShellBridge.h"
#include "ScreenHelperQt.h"
#include "WakeLockListener.h"
#include "mozilla/Hal.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/widget/ScreenManager.h"
#include "gfxPlatform.h"
#include "nsIPowerManagerService.h"
#include "nsServiceManagerUtils.h"
#include "nsWindow.h"
#include "nsXULAppAPI.h"
#include "prenv.h"

#include <cstring>
#include <unistd.h>

#pragma GCC visibility push(default)
#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QSaveFile>
#include <QSize>
#include <QStandardPaths>
#include <QStringList>
#pragma GCC visibility pop

// Defined in nsAppRunner / XRE.
extern const mozilla::XREAppData* gAppData;

#ifdef MOZ_ENABLE_DBUS
#  include <glib.h>
#endif

using namespace mozilla;
using namespace mozilla::widget;

static QApplication* sQtApp = nullptr;
static bool sIdentityPrepared = false;

// Stable argv storage for QApplication (must outlive the app).
static int sQtArgc = 1;
static char sQtArg0[64] = "firefox";
static char* sQtArgv[1] = {sQtArg0};

// Desktop file base name (no ".desktop") used for portals / Wayland app_id.
static char sDesktopFileId[128] = "firefox";

QApplication* nsAppShell::GetQtApp() { return sQtApp; }

/* static */
QApplication* nsAppShell::EnsureQtApp() {
  // Never construct QApplication here — only AppShell::Init may create it.
  if (QApplication* existing =
          qobject_cast<QApplication*>(QApplication::instance())) {
    sQtApp = existing;
    return existing;
  }
  return nullptr;
}

static QString StripDesktopSuffix(QString aName) {
  if (aName.endsWith(QLatin1String(".desktop"), Qt::CaseInsensitive)) {
    aName.chop(8);
  }
  return aName.trimmed();
}

// XRE CharPtr / const char* → QString (Qt6 fromUtf8 needs size for char*).
static QString Utf8(const char* aStr) {
  if (!aStr) {
    return QString();
  }
  return QString::fromUtf8(aStr, qsizetype(std::strlen(aStr)));
}

static const char* ResolveRemotingName() {
  if (const char* envDesktop = PR_GetEnv("MOZ_DESKTOP_FILE_NAME")) {
    // Env may be "firefox" or "firefox.desktop".
    static char sEnvRemoting[128];
    QByteArray base = StripDesktopSuffix(QString::fromLocal8Bit(envDesktop))
                          .toUtf8();
    std::strncpy(sEnvRemoting, base.constData(), sizeof(sEnvRemoting) - 1);
    sEnvRemoting[sizeof(sEnvRemoting) - 1] = '\0';
    return sEnvRemoting;
  }
  if (gAppData && gAppData->remotingName) {
    return gAppData->remotingName;
  }
  return "firefox";
}

static QString DesktopEntryPath(const QString& aBaseName) {
  if (aBaseName.isEmpty()) {
    return QString();
  }
  const QString fileName = aBaseName + QLatin1String(".desktop");
  const QStringList dirs =
      QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
  for (const QString& dir : dirs) {
    const QString path = dir + QLatin1Char('/') + fileName;
    if (QFile::exists(path)) {
      return path;
    }
  }
  return QString();
}

static bool DesktopEntryExists(const QString& aBaseName) {
  return !DesktopEntryPath(aBaseName).isEmpty();
}

static bool IsReadableFile(const QString& aPath) {
  const QFileInfo info(aPath);
  return info.isFile() && info.isReadable();
}

// Desktop Icon= values use a small escape set. Runs before QApplication, so
// this cannot go through QSettings (that would create the app object).
static QString UnescapeDesktopValue(const QString& aValue) {
  QString out;
  out.reserve(aValue.size());
  for (int i = 0; i < aValue.size(); ++i) {
    if (aValue.at(i) == QLatin1Char('\\') && i + 1 < aValue.size()) {
      const QChar next = aValue.at(++i);
      if (next == QLatin1Char('s')) {
        out += QLatin1Char(' ');
      } else if (next == QLatin1Char('n')) {
        out += QLatin1Char('\n');
      } else if (next == QLatin1Char('t')) {
        out += QLatin1Char('\t');
      } else if (next == QLatin1Char('r')) {
        out += QLatin1Char('\r');
      } else {
        out += next;
      }
    } else {
      out += aValue.at(i);
    }
  }
  return out;
}

static QString DesktopEntryIcon(const QString& aPath) {
  QFile file(aPath);
  if (!file.open(QIODevice::ReadOnly)) {
    return QString();
  }
  bool inEntry = false;
  while (!file.atEnd()) {
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
      continue;
    }
    if (line.startsWith(QLatin1Char('['))) {
      inEntry = line.compare(QLatin1String("[Desktop Entry]"),
                             Qt::CaseInsensitive) == 0;
      continue;
    }
    if (!inEntry) {
      continue;
    }
    const int eq = line.indexOf(QLatin1Char('='));
    if (eq <= 0) {
      continue;
    }
    const QString key = line.left(eq).trimmed();
    // Icon[lang]= is a translation, not the icon name.
    if (key.compare(QLatin1String("Icon"), Qt::CaseInsensitive) != 0) {
      continue;
    }
    return UnescapeDesktopValue(line.mid(eq + 1).trimmed());
  }
  return QString();
}

static bool FindNamedIconInDir(const QString& aDir, const QString& aName) {
  static const char* const kExts[] = {".png", ".svg", ".svgz", ".xpm"};
  for (const char* ext : kExts) {
    if (IsReadableFile(aDir + QLatin1Char('/') + aName + QLatin1String(ext))) {
      return true;
    }
  }
  return false;
}

static QStringList IconLookupNames(const QString& aIcon) {
  QStringList names(aIcon);
  static const char* const kExts[] = {".png", ".svg", ".svgz", ".xpm"};
  for (const char* ext : kExts) {
    const QLatin1String suffix(ext);
    if (aIcon.endsWith(suffix, Qt::CaseInsensitive) &&
        aIcon.size() > suffix.size()) {
      names.append(aIcon.left(aIcon.size() - suffix.size()));
    }
  }
  names.removeDuplicates();
  return names;
}

// QIcon::fromTheme needs a QGuiApplication, and this runs before one exists.
// Follow the icon-theme layout enough to tell "installed" from "missing".
static bool ThemeIconExists(const QString& aIcon) {
  if (aIcon.isEmpty()) {
    return false;
  }
  const QStringList names = IconLookupNames(aIcon);
  QStringList roots;
  const QString home = QDir::homePath();
  roots << (home + QLatin1String("/.local/share/icons"))
        << (home + QLatin1String("/.icons"));
  const QStringList dataDirs =
      QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
  for (const QString& dir : dataDirs) {
    roots << (dir + QLatin1String("/icons"))
          << (dir + QLatin1String("/pixmaps"));
  }
  roots.removeDuplicates();

  for (const QString& root : roots) {
    for (const QString& name : names) {
      if (FindNamedIconInDir(root, name)) {
        return true;
      }
    }
  }
  for (const QString& root : roots) {
    const QDir rootDir(root);
    if (!rootDir.exists()) {
      continue;
    }
    const QStringList themes =
        rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& theme : themes) {
      const QString themePath = root + QLatin1Char('/') + theme;
      for (const QString& name : names) {
        if (FindNamedIconInDir(themePath + QLatin1String("/apps"), name)) {
          return true;
        }
      }
      const QDir themeDir(themePath);
      const QStringList sizes =
          themeDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
      for (const QString& size : sizes) {
        const QString apps = themePath + QLatin1Char('/') + size +
                             QLatin1String("/apps");
        for (const QString& name : names) {
          if (FindNamedIconInDir(apps, name)) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

static bool IconNameResolves(const QString& aIcon, const QString& aDesktopPath) {
  if (aIcon.isEmpty() || aIcon == QLatin1String(".") ||
      aIcon == QLatin1String("..")) {
    return false;
  }
  if (aIcon.contains(QLatin1Char('/'))) {
    if (IsReadableFile(aIcon)) {
      return true;
    }
    if (!QFileInfo(aIcon).isAbsolute() && !aDesktopPath.isEmpty()) {
      const QString relative = QFileInfo(aDesktopPath).dir().filePath(aIcon);
      if (IsReadableFile(relative)) {
        return true;
      }
    }
    return false;
  }
  return ThemeIconExists(aIcon);
}

static bool DesktopEntryHasResolvableIcon(const QString& aBaseName) {
  const QString path = DesktopEntryPath(aBaseName);
  if (path.isEmpty()) {
    return false;
  }
  return IconNameResolves(DesktopEntryIcon(path), path);
}

static QString SelfExecutablePath() {
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    return QString::fromLocal8Bit(buf, int(n));
  }
  return QString();
}

static QString BundledIconDir() {
  const QString exe = SelfExecutablePath();
  if (exe.isEmpty()) {
    return QString();
  }
  return QFileInfo(exe).absolutePath() +
         QStringLiteral("/browser/chrome/icons/default/");
}

static QString BundledIconPath() {
  const QString dir = BundledIconDir();
  if (dir.isEmpty()) {
    return QString();
  }
  const QStringList names = {QStringLiteral("default48.png"),
                             QStringLiteral("default256.png"),
                             QStringLiteral("default32.png"),
                             QStringLiteral("default16.png")};
  for (const QString& name : names) {
    const QString path = dir + name;
    if (QFileInfo(path).isFile()) {
      return path;
    }
  }
  return QString();
}

static QIcon BundledWindowIcon() {
  QIcon icon;
  const QString dir = BundledIconDir();
  if (dir.isEmpty()) {
    return icon;
  }
  const struct {
    const char* name;
    int size;
  } files[] = {
      {"default16.png", 16}, {"default22.png", 22}, {"default24.png", 24},
      {"default32.png", 32}, {"default48.png", 48}, {"default256.png", 256},
  };
  for (const auto& file : files) {
    const QString path = dir + QLatin1String(file.name);
    if (QFileInfo(path).isFile()) {
      icon.addFile(path, QSize(file.size, file.size));
    }
  }
  return icon;
}

// xdg-desktop-portal Host Registry requires a resolvable .desktop file.
// Unpackaged / objdir runs often lack one → "App info not found for 'firefox'".
// Install a minimal user-local entry (never overwrite an existing one).
static void EnsureUserDesktopEntry(const QString& aBaseName) {
  if (aBaseName.isEmpty() || DesktopEntryExists(aBaseName)) {
    return;
  }
  const QString appsDir =
      QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
  if (appsDir.isEmpty()) {
    return;
  }
  QDir().mkpath(appsDir);
  const QString path =
      appsDir + QLatin1Char('/') + aBaseName + QLatin1String(".desktop");
  if (QFile::exists(path)) {
    return;
  }

  QString exec = SelfExecutablePath();
  if (exec.isEmpty()) {
    exec = QString::fromUtf8(aBaseName.toUtf8());
  }
  // Escape Exec per desktop-entry spec (\, ", $, `, spaces).
  exec.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
  exec.replace(QLatin1Char('"'), QLatin1String("\\\""));
  exec.replace(QLatin1Char('$'), QLatin1String("\\$"));
  exec.replace(QLatin1Char('`'), QLatin1String("\\`"));
  if (exec.contains(QLatin1Char(' '))) {
    exec = QLatin1Char('"') + exec + QLatin1Char('"');
  }

  const QString displayName =
      (gAppData && gAppData->name) ? Utf8(gAppData->name)
                                   : QStringLiteral("Firefox");

  // Icon=firefox with no theme icon is what leaves a blank panel button.
  // An absolute path is valid in the desktop-entry spec and does not depend
  // on the icon theme knowing our name.
  QString icon = BundledIconPath();
  if (icon.isEmpty()) {
    icon = QStringLiteral("firefox");
  } else {
    icon.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
  }

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return;
  }
  const QByteArray body = QStringLiteral(
                              "[Desktop Entry]\n"
                              "Version=1.0\n"
                              "Type=Application\n"
                              "Name=%1\n"
                              "GenericName=Web Browser\n"
                              "Comment=Browse the Web\n"
                              "Exec=%2 %u\n"
                              "Icon=%3\n"
                              "Terminal=false\n"
                              "Categories=Network;WebBrowser;\n"
                              "MimeType=text/html;text/xml;application/"
                              "xhtml+xml;x-scheme-handler/http;"
                              "x-scheme-handler/https;\n"
                              "StartupNotify=true\n"
                              "StartupWMClass=%4\n"
                              "NoDisplay=false\n")
                              .arg(displayName, exec, icon, aBaseName)
                              .toUtf8();
  if (file.write(body) == body.size()) {
    file.commit();
  }
}

static bool AcceptDesktopId(const QString& aBase) {
  const QByteArray utf8 = aBase.toUtf8();
  if (utf8.isEmpty() || utf8.size() >= qsizetype(sizeof(sDesktopFileId))) {
    return false;
  }
  std::strncpy(sDesktopFileId, utf8.constData(), sizeof(sDesktopFileId) - 1);
  sDesktopFileId[sizeof(sDesktopFileId) - 1] = '\0';
  return true;
}

static QStringList DesktopIdCandidates() {
  // firefox-qt before the generic remoting name ("firefox"). A leftover
  // ~/.local/share/applications/firefox.desktop whose Icon= does not exist
  // must not beat the packaged firefox-qt.desktop that has a real icon.
  QStringList candidates;
  candidates << QStringLiteral("firefox-qt");
  if (gAppData && gAppData->remotingName) {
    candidates << Utf8(gAppData->remotingName);
  }
  candidates << QStringLiteral("firefox")
             << QStringLiteral("org.mozilla.firefox")
             << QStringLiteral("firefox-esr")
             << QStringLiteral("org.mozilla.firefox-esr")
             << QStringLiteral("org.mozilla.Firefox");
  candidates.removeDuplicates();
  return candidates;
}

// Prefer an installed desktop id whose Icon= actually resolves. Plasma's
// task manager draws that icon; a missing name is an empty rounded square.
// Fall back to any existing entry so portals still have an id, then publish
// a user-local entry for unpackaged runs.
static const char* ResolveDesktopFileId() {
  QString envBase;
  if (const char* env = PR_GetEnv("MOZ_DESKTOP_FILE_NAME")) {
    envBase = StripDesktopSuffix(QString::fromLocal8Bit(env));
    if (DesktopEntryHasResolvableIcon(envBase) && AcceptDesktopId(envBase)) {
      return sDesktopFileId;
    }
  }

  const QStringList candidates = DesktopIdCandidates();
  for (const QString& raw : candidates) {
    const QString base = StripDesktopSuffix(raw);
    if (DesktopEntryHasResolvableIcon(base) && AcceptDesktopId(base)) {
      return sDesktopFileId;
    }
  }

  // Icon lookup can miss a theme we don't walk. An explicit env id still wins
  // over a stale firefox.desktop when the file itself exists.
  if (!envBase.isEmpty() && DesktopEntryExists(envBase) &&
      AcceptDesktopId(envBase)) {
    return sDesktopFileId;
  }
  for (const QString& raw : candidates) {
    const QString base = StripDesktopSuffix(raw);
    if (DesktopEntryExists(base) && AcceptDesktopId(base)) {
      return sDesktopFileId;
    }
  }

  const char* remoting = ResolveRemotingName();
  std::strncpy(sDesktopFileId, remoting, sizeof(sDesktopFileId) - 1);
  sDesktopFileId[sizeof(sDesktopFileId) - 1] = '\0';
  EnsureUserDesktopEntry(QString::fromUtf8(sDesktopFileId));
  return sDesktopFileId;
}

// Static identity must be set *before* the first QGuiApplication is
// constructed. Qt's unix portal integration registers app id during
// QGuiApplicationPrivate init; changing it later logs:
// "Could not register app ID: Connection already associated..."
// Never call setDesktopFileName again after QGuiApplication exists.
static void PrepareQtApplicationIdentity() {
  if (sIdentityPrepared) {
    return;
  }
  // If a QGuiApplication already exists, portal registration already ran —
  // do not touch desktopFileName (triggers the "already associated" error).
  if (QCoreApplication::instance()) {
    sIdentityPrepared = true;
    return;
  }
  sIdentityPrepared = true;

  const char* remoting = ResolveRemotingName();
  const char* desktopId = ResolveDesktopFileId();
  const char* displayName =
      (gAppData && gAppData->name) ? gAppData->name : "Firefox";
  const char* vendor =
      (gAppData && gAppData->vendor) ? gAppData->vendor : "Mozilla";

  std::strncpy(sQtArg0, remoting, sizeof(sQtArg0) - 1);
  sQtArg0[sizeof(sQtArg0) - 1] = '\0';

  QCoreApplication::setOrganizationName(Utf8(vendor));
  QCoreApplication::setApplicationName(Utf8(remoting));
  QGuiApplication::setApplicationDisplayName(Utf8(displayName));
  // Portal + Wayland app_id: must match a resolvable .desktop basename.
  QGuiApplication::setDesktopFileName(Utf8(desktopId));
}

static bool ThemeIconHasPixels(const QIcon& aIcon) {
  if (aIcon.isNull()) {
    return false;
  }
  // A named theme icon can be non-null before its files are resolved.
  // Scalable icons report no fixed sizes, so also ask for a pixmap.
  if (!aIcon.availableSizes().isEmpty()) {
    return true;
  }
  return !aIcon.pixmap(48).isNull();
}

static void ApplyQtApplicationIcon(QApplication* aApp) {
  if (!aApp) {
    return;
  }
  QStringList names;
  names << Utf8(sDesktopFileId[0] ? sDesktopFileId : ResolveRemotingName())
        << QStringLiteral("firefox-qt") << QStringLiteral("firefox")
        << QStringLiteral("firefox-esr")
        << QStringLiteral("org.mozilla.firefox");
  names.removeDuplicates();
  for (const QString& name : names) {
    if (name.isEmpty()) {
      continue;
    }
    const QIcon theme = QIcon::fromTheme(name);
    if (ThemeIconHasPixels(theme)) {
      aApp->setWindowIcon(theme);
      return;
    }
  }
  const QIcon bundled = BundledWindowIcon();
  if (!bundled.isNull()) {
    aApp->setWindowIcon(bundled);
  }
}

nsAppShell::~nsAppShell() {
  mQuit = true;

  mozilla::hal::Shutdown();

  // Drop screen helper first — it is a QObject connected to QGuiApplication
  // signals and must not outlive (or receive events during) QApplication
  // teardown.
  if (XRE_IsParentProcess()) {
    ScreenManager::GetSingleton().SetHelper(nullptr);
  }

  // Clear the bridge before any processEvents / QApplication delete so
  // queued WakeEventLoop slots cannot call back into a dying AppShell.
  if (mBridge) {
    mBridge->disconnect();
    delete mBridge;
    mBridge = nullptr;
  }

  if (sQtApp == mApp) {
    sQtApp = nullptr;
  }

  if (mOwnsApp && mApp) {
    // Do not processEvents() here: module shutdown order destroys LookAndFeel
    // before AppShell, and deferred QTimers/filters can re-enter freed state.
    //
    // Do NOT destroy QWindows here. nsWindow owns each QtWindowBridge via
    // UniquePtr; calling destroy() races that ownership and can segfault on
    // exit (RunWatchdog / nested loops during BackgroundThreadPool shutdown).
    // Gecko widget teardown should already have hidden/destroyed them.
    mApp->quit();
    delete mApp;
  }
  mApp = nullptr;
  mOwnsApp = false;
}

nsresult nsAppShell::Init() {
  mozilla::hal::Init();

  if (!QApplication::instance()) {
    if (gfxPlatform::IsHeadless() &&
        qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
      qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }

    // Identity BEFORE construction — critical for portal registration.
    if (XRE_IsParentProcess()) {
      PrepareQtApplicationIdentity();
    }

    mApp = new QApplication(sQtArgc, sQtArgv);
    mOwnsApp = true;
    sQtApp = mApp;

    if (XRE_IsParentProcess()) {
      ApplyQtApplicationIcon(mApp);
    }
  } else {
    mApp = qobject_cast<QApplication*>(QApplication::instance());
    mOwnsApp = false;
    sQtApp = mApp;
  }

  if (!mApp) {
    return NS_ERROR_FAILURE;
  }

  mBridge = new QtAppShellBridge(this);

  if (XRE_IsParentProcess()) {
    ScreenManager& screenManager = ScreenManager::GetSingleton();
    if (gfxPlatform::IsHeadless()) {
      screenManager.SetHelper(mozilla::MakeUnique<HeadlessScreenHelper>());
    } else {
      screenManager.SetHelper(mozilla::MakeUnique<ScreenHelperQt>());
    }
  }

  return nsBaseAppShell::Init();
}

static mozilla::StaticRefPtr<mozilla::widget::WakeLockListener> sWakeLockListener;

static void AddScreenWakeLockListener() {
  nsCOMPtr<nsIPowerManagerService> powerManager =
      do_GetService(POWERMANAGERSERVICE_CONTRACTID);
  if (powerManager) {
    sWakeLockListener = new mozilla::widget::WakeLockListener();
    powerManager->AddWakeLockListener(sWakeLockListener);
  } else {
    NS_WARNING(
        "Failed to retrieve PowerManagerService, wakelocks will be broken!");
  }
}

static void RemoveScreenWakeLockListener() {
  nsCOMPtr<nsIPowerManagerService> powerManager =
      do_GetService(POWERMANAGERSERVICE_CONTRACTID);
  if (powerManager && sWakeLockListener) {
    powerManager->RemoveWakeLockListener(sWakeLockListener);
  }
  sWakeLockListener = nullptr;
}

NS_IMETHODIMP
nsAppShell::Run() {
  if (XRE_IsParentProcess()) {
    AddScreenWakeLockListener();
  }
  nsresult rv = nsBaseAppShell::Run();
  if (XRE_IsParentProcess()) {
    RemoveScreenWakeLockListener();
  }
  return rv;
}

NS_IMETHODIMP
nsAppShell::Exit() {
  // Stop native waiting immediately so shutdown is not stuck in processEvents
  // while XPCOM tears down BackgroundThreadPool (RunWatchdog nested-loop hang).
  mQuit = true;
  if (sQtApp) {
    sQtApp->quit();
  }
  return nsBaseAppShell::Exit();
}

void nsAppShell::ScheduleNativeEventCallback() {
  if (mBridge && sQtApp && !mQuit) {
    Q_EMIT mBridge->WakeEventLoop();
  }
}

void nsAppShell::OnQuitApplication() {
  mQuit = true;
  if (sQtApp) {
    sQtApp->quit();
  }
}

bool nsAppShell::ProcessNextNativeEvent(bool mayWait) {
  if (mSuspendNativeCount || !sQtApp || mQuit) {
    return false;
  }

  QAbstractEventDispatcher* dispatcher = QAbstractEventDispatcher::instance();
  if (!dispatcher) {
    return false;
  }

  // Pump GLib so libwebrtc PipeWire/portal and any GLib-based helpers can
  // progress when Qt is not using the GLib event dispatcher. Cap iterations
  // so a busy GLib source cannot starve Qt/Gecko.
#ifdef MOZ_ENABLE_DBUS
  {
    constexpr int kMaxGlibIters = 8;
    int n = 0;
    while (n < kMaxGlibIters && g_main_context_iteration(nullptr, FALSE)) {
      ++n;
    }
  }
#endif

  // Parent main thread: mozilla::ipc::MessagePump -> NS_ProcessNextEvent ->
  // nsBaseAppShell::OnProcessNextEvent, which loops:
  //   while (!HasPendingEvents && !mProcessedGeckoEvents)
  //     ProcessNextNativeEvent(mayWait)
  // as long as we return true.
  //
  // Qt's processEvents() often returns true for internal bookkeeping even
  // when there is nothing useful to do (Wayland frame callbacks, QPA timers,
  // D-Bus). Returning that value here spins the appshell forever and never
  // hands control back to XPCOM/TaskController — empty canvas (present
  // runnables never run), dead input, and ~one-core CPU.
  //
  // Drain one native batch (blocking only when mayWait and the queue is
  // empty). Always return false so XPCOM can run; WakeEventLoop re-enters
  // when other threads post work, and DummyEvent covers the idle path.
  QEventLoop::ProcessEventsFlags flags = QEventLoop::AllEvents;
  if (mayWait) {
    flags |= QEventLoop::WaitForMoreEvents;
  }
  dispatcher->processEvents(flags);
  return false;
}
