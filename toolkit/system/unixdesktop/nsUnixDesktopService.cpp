/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsUnixDesktopService.h"

#include "mozilla/Logging.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/widget/PortalUtils.h"
#include "nsArray.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsIFile.h"
#include "nsIMutableArray.h"
#include "nsIURI.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsStringEnumerator.h"
#include "nsTArray.h"
#include "nsTHashtable.h"
#include "nsHashKeys.h"
#include "nsThreadUtils.h"
#include "prenv.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#pragma GCC visibility push(default)
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusObjectPath>
#include <QtDBus/QDBusReply>
#include <QVariantMap>
#pragma GCC visibility pop

#ifdef MOZ_ENABLE_KF6_SERVICE
#  include <KApplicationTrader>
#  include <KService>
#endif

extern char** environ;

using namespace mozilla;

static LazyLogModule gUnixDesktopLog("UnixDesktop");
#define LOG(...) \
  MOZ_LOG(gUnixDesktopLog, LogLevel::Debug, (__VA_ARGS__))

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

static nsresult RunProcessCapture(const char* aExecutable,
                                  const nsTArray<nsCString>& aArgs,
                                  nsACString* aStdout, int* aExitStatus) {
  if (aExitStatus) {
    *aExitStatus = -1;
  }
  if (aStdout) {
    aStdout->Truncate();
  }

  nsTArray<const char*> argv;
  argv.AppendElement(aExecutable);
  for (const auto& arg : aArgs) {
    argv.AppendElement(arg.get());
  }
  argv.AppendElement(nullptr);

  int pipefd[2] = {-1, -1};
  if (aStdout && pipe(pipefd) != 0) {
    return NS_ERROR_FAILURE;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (aStdout) {
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  } else {
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
  }
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                   O_WRONLY, 0);

  pid_t pid = 0;
  const int spawnRv =
      posix_spawnp(&pid, aExecutable, &actions, nullptr,
                   const_cast<char* const*>(argv.Elements()), environ);
  posix_spawn_file_actions_destroy(&actions);

  if (aStdout) {
    close(pipefd[1]);
  }

  if (spawnRv != 0) {
    if (aStdout) {
      close(pipefd[0]);
    }
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (aStdout) {
    char buf[512];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
      aStdout->Append(buf, n);
    }
    close(pipefd[0]);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return NS_ERROR_FAILURE;
  }
  if (aExitStatus) {
    *aExitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
  return NS_OK;
}

static nsresult RunProcessDetached(const nsTArray<nsCString>& aArgv) {
  if (aArgv.IsEmpty()) {
    return NS_ERROR_FAILURE;
  }
  QStringList args;
  for (size_t i = 1; i < aArgv.Length(); ++i) {
    args << QString::fromUtf8(aArgv[i].get());
  }
  const bool ok = QProcess::startDetached(
      QString::fromUtf8(aArgv[0].get()), args);
  return ok ? NS_OK : NS_ERROR_FAILURE;
}

static bool IsKdeDesktop() {
  const char* desktop = PR_GetEnv("XDG_CURRENT_DESKTOP");
  if (desktop && strstr(desktop, "KDE")) {
    return true;
  }
  const char* session = PR_GetEnv("DESKTOP_SESSION");
  if (session && (strstr(session, "plasma") || strstr(session, "kde"))) {
    return true;
  }
  return false;
}

static bool CommandExists(const char* aCmd) {
  nsTArray<nsCString> args;
  // `command -v` is shell; use access on common paths or which via spawn.
  // QStandardPaths::findExecutable is reliable with Qt.
  return !QStandardPaths::findExecutable(QString::fromUtf8(aCmd)).isEmpty();
}

// ---------------------------------------------------------------------------
// Desktop file model
// ---------------------------------------------------------------------------

struct DesktopEntry {
  nsCString id;    // basename including .desktop
  nsCString path;  // absolute path to the file
  nsCString name;
  nsCString genericName;
  nsCString comment;
  nsCString exec;
  nsCString icon;
  nsCString tryExec;
  bool hidden = false;
  bool noDisplay = false;
  bool expectsUris = false;
  nsTArray<nsCString> mimeTypes;
};

static void TrimInPlace(nsACString& aStr) {
  aStr.Trim(" \t\r\n", true, true);
}

static bool SplitKeyValue(const nsACString& aLine, nsACString& aKey,
                          nsACString& aValue) {
  int32_t eq = aLine.FindChar('=');
  if (eq <= 0) {
    return false;
  }
  aKey = Substring(aLine, 0, eq);
  aValue = Substring(aLine, eq + 1);
  TrimInPlace(aKey);
  // Values may intentionally have leading spaces after '=' in rare cases;
  // FreeDesktop allows trimming.
  TrimInPlace(aValue);
  return !aKey.IsEmpty();
}

static bool ParseDesktopFile(const nsACString& aPath, const nsACString& aId,
                             DesktopEntry& aOut) {
  QFile file(QString::fromUtf8(PromiseFlatCString(aPath).get()));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return false;
  }

  aOut = DesktopEntry();
  aOut.id = aId;
  aOut.path = aPath;

  bool inDesktopEntry = false;
  bool gotName = false;
  while (!file.atEnd()) {
    QByteArray raw = file.readLine();
    nsAutoCString line(raw.constData(), raw.size());
    TrimInPlace(line);
    if (line.IsEmpty() || line[0] == '#') {
      continue;
    }
    if (line[0] == '[') {
      inDesktopEntry = line.EqualsLiteral("[Desktop Entry]");
      continue;
    }
    if (!inDesktopEntry) {
      continue;
    }

    nsAutoCString key, value;
    if (!SplitKeyValue(line, key, value)) {
      continue;
    }

    if (key.EqualsLiteral("Type") && !value.EqualsLiteral("Application")) {
      return false;
    }
    if (key.EqualsLiteral("Name") && !gotName) {
      aOut.name = value;
      gotName = true;
    } else if (key.EqualsLiteral("GenericName") && aOut.genericName.IsEmpty()) {
      aOut.genericName = value;
    } else if (key.EqualsLiteral("Comment") && aOut.comment.IsEmpty()) {
      aOut.comment = value;
    } else if (key.EqualsLiteral("Exec")) {
      aOut.exec = value;
      if (value.Find("%u") != kNotFound || value.Find("%U") != kNotFound) {
        aOut.expectsUris = true;
      }
    } else if (key.EqualsLiteral("Icon")) {
      aOut.icon = value;
    } else if (key.EqualsLiteral("TryExec")) {
      aOut.tryExec = value;
    } else if (key.EqualsLiteral("Hidden")) {
      aOut.hidden = value.EqualsLiteral("true");
    } else if (key.EqualsLiteral("NoDisplay")) {
      aOut.noDisplay = value.EqualsLiteral("true");
    } else if (key.EqualsLiteral("MimeType")) {
      // semicolon-separated
      nsCString rest(value);
      while (!rest.IsEmpty()) {
        int32_t semi = rest.FindChar(';');
        nsAutoCString part;
        if (semi == kNotFound) {
          part = rest;
          rest.Truncate();
        } else {
          part = Substring(rest, 0, semi);
          rest = Substring(rest, semi + 1);
        }
        TrimInPlace(part);
        if (!part.IsEmpty()) {
          aOut.mimeTypes.AppendElement(part);
        }
      }
    }
  }

  if (aOut.hidden || aOut.exec.IsEmpty() || aOut.name.IsEmpty()) {
    return false;
  }
  if (!aOut.tryExec.IsEmpty() &&
      QStandardPaths::findExecutable(
          QString::fromUtf8(aOut.tryExec.get()))
          .isEmpty()) {
    // TryExec not in PATH — still keep if absolute and exists.
    if (aOut.tryExec[0] == '/') {
      if (!QFileInfo::exists(QString::fromUtf8(aOut.tryExec.get()))) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

static void CollectApplicationDirs(nsTArray<nsCString>& aDirs) {
  // Precedence: highest first for defaults, but for discovery we scan all.
  auto appendDir = [&](const QString& base) {
    if (base.isEmpty()) {
      return;
    }
    QString apps = base + QStringLiteral("/applications");
    nsAutoCString path;
    path.Assign(apps.toUtf8().constData());
    if (!aDirs.Contains(path)) {
      aDirs.AppendElement(path);
    }
  };

  const char* home = PR_GetEnv("XDG_DATA_HOME");
  if (home && *home) {
    appendDir(QString::fromLocal8Bit(home));
  } else if (const char* h = PR_GetEnv("HOME")) {
    appendDir(QString::fromLocal8Bit(h) + QStringLiteral("/.local/share"));
  }

  const char* dirs = PR_GetEnv("XDG_DATA_DIRS");
  QStringList list = dirs && *dirs
                         ? QString::fromLocal8Bit(dirs).split(
                               QLatin1Char(':'), Qt::SkipEmptyParts)
                         : QStringList{QStringLiteral("/usr/local/share"),
                                       QStringLiteral("/usr/share")};
  for (const QString& d : list) {
    appendDir(d);
  }
}

static bool FindDesktopById(const nsACString& aId, DesktopEntry& aOut) {
  nsAutoCString id(aId);
  if (!StringEndsWith(id, ".desktop"_ns)) {
    id.AppendLiteral(".desktop");
  }

  nsTArray<nsCString> dirs;
  CollectApplicationDirs(dirs);

  // Fast path: direct path in each applications dir.
  for (const auto& dir : dirs) {
    nsAutoCString path(dir);
    path.Append('/');
    path.Append(id);
    if (ParseDesktopFile(path, id, aOut)) {
      return true;
    }
  }

  // Recursive basename match (vendor subdirectories).
  for (const auto& dir : dirs) {
    QDirIterator it(QString::fromUtf8(dir.get()),
                    QStringList() << QStringLiteral("*.desktop"), QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      QFileInfo fi = it.fileInfo();
      if (fi.fileName().toUtf8() != id.get()) {
        continue;
      }
      nsAutoCString path;
      path.Assign(fi.absoluteFilePath().toUtf8().constData());
      if (ParseDesktopFile(path, id, aOut)) {
        return true;
      }
    }
  }
  return false;
}

static void CollectDesktopEntriesForMime(const nsACString& aMime,
                                         nsTArray<DesktopEntry>& aOut) {
  nsTArray<nsCString> dirs;
  CollectApplicationDirs(dirs);
  nsTHashtable<nsCStringHashKey> seen;

  auto considerFile = [&](const nsACString& path, const nsACString& id) {
    if (seen.Contains(id)) {
      return;
    }
    DesktopEntry entry;
    if (!ParseDesktopFile(path, id, entry)) {
      return;
    }
    if (entry.noDisplay && !entry.mimeTypes.Contains(aMime)) {
      return;
    }
    if (!entry.mimeTypes.Contains(aMime)) {
      return;
    }
    seen.PutEntry(id);
    aOut.AppendElement(std::move(entry));
  };

  for (const auto& dir : dirs) {
    QDir qdir(QString::fromUtf8(dir.get()));
    if (!qdir.exists()) {
      continue;
    }
    QDirIterator it(qdir.path(), QStringList() << QStringLiteral("*.desktop"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      QFileInfo fi = it.fileInfo();
      nsAutoCString id;
      id.Assign(fi.fileName().toUtf8().constData());
      nsAutoCString path;
      path.Assign(fi.absoluteFilePath().toUtf8().constData());
      considerFile(path, id);
    }
  }
}

// ---------------------------------------------------------------------------
// mimeapps.list / xdg-mime / KDE defaults
// ---------------------------------------------------------------------------

static void CollectMimeappsPaths(nsTArray<nsCString>& aPaths) {
  auto add = [&](const QString& p) {
    if (p.isEmpty() || !QFileInfo::exists(p)) {
      return;
    }
    nsAutoCString path;
    path.Assign(p.toUtf8().constData());
    if (!aPaths.Contains(path)) {
      aPaths.AppendElement(path);
    }
  };

  const char* cfgHome = PR_GetEnv("XDG_CONFIG_HOME");
  QString configHome;
  if (cfgHome && *cfgHome) {
    configHome = QString::fromLocal8Bit(cfgHome);
  } else if (const char* h = PR_GetEnv("HOME")) {
    configHome = QString::fromLocal8Bit(h) + QStringLiteral("/.config");
  }
  if (!configHome.isEmpty()) {
    add(configHome + QStringLiteral("/mimeapps.list"));
    add(configHome + QStringLiteral("/kde-mimeapps.list"));
  }

  const char* cfgDirs = PR_GetEnv("XDG_CONFIG_DIRS");
  QStringList configs =
      cfgDirs && *cfgDirs
          ? QString::fromLocal8Bit(cfgDirs).split(QLatin1Char(':'),
                                                  Qt::SkipEmptyParts)
          : QStringList{QStringLiteral("/etc/xdg")};
  for (const QString& d : configs) {
    add(d + QStringLiteral("/mimeapps.list"));
    add(d + QStringLiteral("/kde-mimeapps.list"));
  }

  nsTArray<nsCString> dataDirs;
  CollectApplicationDirs(dataDirs);
  for (const auto& d : dataDirs) {
    nsAutoCString p(d);
    p.AppendLiteral("/mimeapps.list");
    if (QFileInfo::exists(QString::fromUtf8(p.get()))) {
      if (!aPaths.Contains(p)) {
        aPaths.AppendElement(p);
      }
    }
  }
}

static bool LookupMimeappsDefault(const nsACString& aMime, nsACString& aDesktopId) {
  nsTArray<nsCString> paths;
  CollectMimeappsPaths(paths);
  for (const auto& path : paths) {
    QFile file(QString::fromUtf8(path.get()));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      continue;
    }
    bool inDefaults = false;
    while (!file.atEnd()) {
      QByteArray raw = file.readLine();
      nsAutoCString line(raw.constData(), raw.size());
      TrimInPlace(line);
      if (line.IsEmpty() || line[0] == '#') {
        continue;
      }
      if (line[0] == '[') {
        inDefaults = line.EqualsLiteral("[Default Applications]");
        continue;
      }
      if (!inDefaults) {
        continue;
      }
      nsAutoCString key, value;
      if (!SplitKeyValue(line, key, value)) {
        continue;
      }
      if (!key.Equals(aMime)) {
        continue;
      }
      // value may be desktop1;desktop2;
      int32_t semi = value.FindChar(';');
      nsAutoCString first;
      if (semi == kNotFound) {
        first = value;
      } else {
        first.Assign(Substring(value, 0, semi));
      }
      TrimInPlace(first);
      if (!first.IsEmpty()) {
        aDesktopId = first;
        return true;
      }
    }
  }
  return false;
}

static void LookupMimeappsAdded(const nsACString& aMime,
                                nsTArray<nsCString>& aDesktopIds) {
  nsTArray<nsCString> paths;
  CollectMimeappsPaths(paths);
  // Later files are lower precedence; first-seen wins for order of discovery
  // but we want union. Prefer defaults-first order separately.
  for (const auto& path : paths) {
    QFile file(QString::fromUtf8(path.get()));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      continue;
    }
    bool inAdded = false;
    while (!file.atEnd()) {
      QByteArray raw = file.readLine();
      nsAutoCString line(raw.constData(), raw.size());
      TrimInPlace(line);
      if (line.IsEmpty() || line[0] == '#') {
        continue;
      }
      if (line[0] == '[') {
        inAdded = line.EqualsLiteral("[Added Associations]");
        continue;
      }
      if (!inAdded) {
        continue;
      }
      nsAutoCString key, value;
      if (!SplitKeyValue(line, key, value) || !key.Equals(aMime)) {
        continue;
      }
      nsCString rest(value);
      while (!rest.IsEmpty()) {
        int32_t semi = rest.FindChar(';');
        nsAutoCString part;
        if (semi == kNotFound) {
          part = rest;
          rest.Truncate();
        } else {
          part = Substring(rest, 0, semi);
          rest = Substring(rest, semi + 1);
        }
        TrimInPlace(part);
        if (!part.IsEmpty() && !aDesktopIds.Contains(part)) {
          aDesktopIds.AppendElement(part);
        }
      }
    }
  }
}

static bool XdgMimeQueryDefault(const nsACString& aMime, nsACString& aDesktopId) {
  nsTArray<nsCString> args;
  args.AppendElement("query"_ns);
  args.AppendElement("default"_ns);
  args.AppendElement(nsCString(aMime));
  nsAutoCString out;
  int exitStatus = -1;
  if (NS_FAILED(RunProcessCapture("xdg-mime", args, &out, &exitStatus)) ||
      exitStatus != 0) {
    return false;
  }
  out.Trim(" \t\r\n", true, true);
  if (out.IsEmpty()) {
    return false;
  }
  aDesktopId = out;
  return true;
}

static bool KdeReadConfigKey(const char* aGroup, const char* aKey,
                             nsACString& aValue) {
  const char* bin = nullptr;
  if (CommandExists("kreadconfig6")) {
    bin = "kreadconfig6";
  } else if (CommandExists("kreadconfig5")) {
    bin = "kreadconfig5";
  } else {
    return false;
  }
  nsTArray<nsCString> args;
  args.AppendElement("--file"_ns);
  args.AppendElement("kdeglobals"_ns);
  args.AppendElement("--group"_ns);
  args.AppendElement(nsDependentCString(aGroup));
  args.AppendElement("--key"_ns);
  args.AppendElement(nsDependentCString(aKey));
  nsAutoCString out;
  int exitStatus = -1;
  if (NS_FAILED(RunProcessCapture(bin, args, &out, &exitStatus)) ||
      exitStatus != 0) {
    return false;
  }
  out.Trim(" \t\r\n", true, true);
  if (out.IsEmpty()) {
    return false;
  }
  aValue = out;
  return true;
}

static bool ResolveDefaultDesktopIdForMime(const nsACString& aMime,
                                           nsACString& aDesktopId) {
  // KDE-centric shortcuts for common scheme handlers.
  if (aMime.EqualsLiteral("x-scheme-handler/http") ||
      aMime.EqualsLiteral("x-scheme-handler/https") ||
      aMime.EqualsLiteral("text/html")) {
    nsAutoCString browser;
    if (KdeReadConfigKey("General", "BrowserApplication", browser)) {
      if (!StringEndsWith(browser, ".desktop"_ns) && browser.FindChar('/') == kNotFound) {
        // Sometimes stored without suffix
        if (!browser.EqualsLiteral("chromium") && browser.Find(" "_ns) == kNotFound) {
          // keep as-is; may already include .desktop
        }
      }
      if (!browser.IsEmpty()) {
        if (!StringEndsWith(browser, ".desktop"_ns) && browser.FindChar('.') == kNotFound) {
          browser.AppendLiteral(".desktop");
        }
        // BrowserApplication may be a binary name; only accept if .desktop found
        DesktopEntry e;
        if (StringEndsWith(browser, ".desktop"_ns) && FindDesktopById(browser, e)) {
          aDesktopId = browser;
          return true;
        }
        if (StringEndsWith(browser, ".desktop"_ns)) {
          aDesktopId = browser;
          return true;
        }
      }
    }
  }
  if (aMime.EqualsLiteral("x-scheme-handler/mailto")) {
    nsAutoCString email;
    if (KdeReadConfigKey("General", "EmailClient", email)) {
      email.Trim(" \t\r\n", true, true);
      if (!email.IsEmpty()) {
        if (!StringEndsWith(email, ".desktop"_ns)) {
          // EmailClient can be a full command; try as desktop id if simple.
          if (email.Find(" "_ns) == kNotFound && email.FindChar('/') == kNotFound) {
            email.AppendLiteral(".desktop");
          }
        }
        if (StringEndsWith(email, ".desktop"_ns)) {
          aDesktopId = email;
          return true;
        }
      }
    }
  }

  if (XdgMimeQueryDefault(aMime, aDesktopId)) {
    return true;
  }
  return LookupMimeappsDefault(aMime, aDesktopId);
}

// ---------------------------------------------------------------------------
// Exec expansion and launch
// ---------------------------------------------------------------------------

static nsresult ExpandAndLaunchExec(const nsACString& aExec,
                                    const nsACString& aName,
                                    const nsACString& aIcon,
                                    const nsACString& aDesktopPath,
                                    const nsACString& aUriOrPath,
                                    bool aIsFilePath) {
  // Tokenize Exec respecting simple quoting.
  nsTArray<nsCString> tokens;
  {
    nsAutoCString exec(aExec);
    nsAutoCString cur;
    bool inSingle = false, inDouble = false;
    for (uint32_t i = 0; i < exec.Length(); ++i) {
      char c = exec[i];
      if (inSingle) {
        if (c == '\'') {
          inSingle = false;
        } else {
          cur.Append(c);
        }
        continue;
      }
      if (inDouble) {
        if (c == '"') {
          inDouble = false;
        } else if (c == '\\' && i + 1 < exec.Length()) {
          cur.Append(exec[++i]);
        } else {
          cur.Append(c);
        }
        continue;
      }
      if (c == '\'') {
        inSingle = true;
      } else if (c == '"') {
        inDouble = true;
      } else if (c == '\\' && i + 1 < exec.Length()) {
        cur.Append(exec[++i]);
      } else if (c == ' ' || c == '\t') {
        if (!cur.IsEmpty()) {
          tokens.AppendElement(cur);
          cur.Truncate();
        }
      } else {
        cur.Append(c);
      }
    }
    if (!cur.IsEmpty()) {
      tokens.AppendElement(cur);
    }
  }
  if (tokens.IsEmpty()) {
    return NS_ERROR_FAILURE;
  }

  nsAutoCString uri(aUriOrPath);
  nsAutoCString filePath;
  if (aIsFilePath) {
    filePath = uri;
    // file URI form for %u
    uri = "file://"_ns;
    // crude path encode: leave as-is for local UTF-8 paths
    uri.Append(filePath);
  } else {
    // If it's a file: URI, extract path for %f
    if (StringBeginsWith(uri, "file:"_ns)) {
      QUrl qurl(QString::fromUtf8(uri.get()));
      filePath.Assign(qurl.toLocalFile().toUtf8().constData());
    }
  }

  bool usedFieldCode = false;
  nsTArray<nsCString> argv;
  for (const auto& tok : tokens) {
    if (tok.EqualsLiteral("%f") || tok.EqualsLiteral("%F")) {
      usedFieldCode = true;
      if (!filePath.IsEmpty()) {
        argv.AppendElement(filePath);
      } else if (!aIsFilePath) {
        // URI given but %f expected — pass as-is
        argv.AppendElement(nsCString(aUriOrPath));
      }
    } else if (tok.EqualsLiteral("%u") || tok.EqualsLiteral("%U")) {
      usedFieldCode = true;
      argv.AppendElement(uri);
    } else if (tok.EqualsLiteral("%i")) {
      if (!aIcon.IsEmpty()) {
        argv.AppendElement("--icon"_ns);
        argv.AppendElement(nsCString(aIcon));
      }
    } else if (tok.EqualsLiteral("%c")) {
      argv.AppendElement(nsCString(aName));
    } else if (tok.EqualsLiteral("%k")) {
      argv.AppendElement(nsCString(aDesktopPath));
    } else if (tok.Length() == 2 && tok[0] == '%') {
      // Deprecated / ignored field codes
      continue;
    } else {
      // Embedded field codes inside a token
      nsAutoCString expanded;
      for (uint32_t i = 0; i < tok.Length(); ++i) {
        if (tok[i] == '%' && i + 1 < tok.Length()) {
          char code = tok[i + 1];
          if (code == '%') {
            expanded.Append('%');
            ++i;
            continue;
          }
          usedFieldCode = true;
          if (code == 'u' || code == 'U') {
            expanded.Append(uri);
            ++i;
            continue;
          }
          if (code == 'f' || code == 'F') {
            expanded.Append(filePath.IsEmpty() ? nsCString(aUriOrPath)
                                               : filePath);
            ++i;
            continue;
          }
          if (code == 'c') {
            expanded.Append(aName);
            ++i;
            continue;
          }
          if (code == 'k') {
            expanded.Append(aDesktopPath);
            ++i;
            continue;
          }
          // skip unknown
          ++i;
          continue;
        }
        expanded.Append(tok[i]);
      }
      argv.AppendElement(expanded);
    }
  }

  if (!usedFieldCode && !aUriOrPath.IsEmpty()) {
    argv.AppendElement(nsCString(aUriOrPath));
  }

  LOG("ExpandAndLaunchExec: %s", argv[0].get());
  return RunProcessDetached(argv);
}

static bool PreferOpenUriPortal() {
  return mozilla::widget::ShouldUsePortal(
      mozilla::widget::PortalKind::OpenUri);
}

static bool PreferMimeHandlerPortal() {
  return mozilla::widget::ShouldUsePortal(
      mozilla::widget::PortalKind::MimeHandler);
}

static bool OpenWithXdgPortal(const nsACString& aUriOrPath);
static nsresult OpenWithSystemDefault(const nsACString& aUriOrPath);

// SchemeSupported via org.freedesktop.portal.OpenURI (QtDBus, no GIO).
static bool PortalSchemeSupported(const nsACString& aScheme) {
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QStringLiteral("org.freedesktop.portal.Desktop"),
      QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.OpenURI"),
      QStringLiteral("SchemeSupported"));
  QVariantMap options;
  msg << QString::fromUtf8(PromiseFlatCString(aScheme).get()) << options;
  const QDBusMessage reply =
      QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
  if (reply.type() != QDBusMessage::ReplyMessage ||
      reply.arguments().isEmpty()) {
    // Older portals lack SchemeSupported — be optimistic (flatpak path).
    return true;
  }
  return reply.arguments().at(0).toBool();
}

// Portal-backed handler when sandboxed (SchemeSupported / OpenURI).
class nsPortalHandlerApp final : public nsIHandlerApp {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIHANDLERAPP
  nsPortalHandlerApp() = default;

 private:
  ~nsPortalHandlerApp() = default;
};

NS_IMPL_ISUPPORTS(nsPortalHandlerApp, nsIHandlerApp)

NS_IMETHODIMP
nsPortalHandlerApp::GetName(nsAString& aName) {
  aName.AssignLiteral("System Handler");
  return NS_OK;
}

NS_IMETHODIMP
nsPortalHandlerApp::SetName(const nsAString&) { return NS_OK; }

NS_IMETHODIMP
nsPortalHandlerApp::GetDetailedDescription(nsAString&) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsPortalHandlerApp::SetDetailedDescription(const nsAString&) {
  return NS_OK;
}

NS_IMETHODIMP
nsPortalHandlerApp::Equals(nsIHandlerApp* aOther, bool* aRet) {
  *aRet = false;
  nsCOMPtr<nsIHandlerApp> other = aOther;
  *aRet = other && (other.get() == static_cast<nsIHandlerApp*>(this));
  return NS_OK;
}

NS_IMETHODIMP
nsPortalHandlerApp::LaunchWithURI(nsIURI* aUri,
                                  mozilla::dom::BrowsingContext*) {
  if (!aUri) {
    return NS_ERROR_INVALID_ARG;
  }
  nsAutoCString spec;
  nsresult rv = aUri->GetSpec(spec);
  NS_ENSURE_SUCCESS(rv, rv);
  if (OpenWithXdgPortal(spec)) {
    return NS_OK;
  }
  return OpenWithSystemDefault(spec);
}

static bool OpenWithXdgPortal(const nsACString& aUriOrPath) {
  // org.freedesktop.portal.OpenURI.OpenURI / OpenFile
  QDBusInterface portal(
      QStringLiteral("org.freedesktop.portal.Desktop"),
      QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.OpenURI"),
      QDBusConnection::sessionBus());
  if (!portal.isValid()) {
    return false;
  }
  QString target = QString::fromUtf8(PromiseFlatCString(aUriOrPath).get());
  QVariantMap options;
  // Empty parent window handle — portal backends accept "".
  QDBusReply<QDBusObjectPath> reply;
  if (target.startsWith(QLatin1Char('/')) ||
      target.startsWith(QStringLiteral("file:"))) {
    // OpenFile expects a file descriptor; fall through to OpenURI with file://.
    if (target.startsWith(QLatin1Char('/'))) {
      target = QUrl::fromLocalFile(target).toString(QUrl::FullyEncoded);
    }
  }
  reply = portal.call(QStringLiteral("OpenURI"), QString(), target, options);
  return reply.isValid();
}

static nsresult OpenWithSystemDefault(const nsACString& aUriOrPath) {
  if (PreferOpenUriPortal() && OpenWithXdgPortal(aUriOrPath)) {
    return NS_OK;
  }
  // Prefer KDE kioclient when on Plasma — uses KIO and respects KDE defaults.
  if (IsKdeDesktop() && CommandExists("kioclient")) {
    nsTArray<nsCString> argv;
    argv.AppendElement("kioclient"_ns);
    argv.AppendElement("open"_ns);
    argv.AppendElement(nsCString(aUriOrPath));
    if (NS_SUCCEEDED(RunProcessDetached(argv))) {
      return NS_OK;
    }
  }
  if (CommandExists("xdg-open")) {
    nsTArray<nsCString> argv;
    argv.AppendElement("xdg-open"_ns);
    argv.AppendElement(nsCString(aUriOrPath));
    if (NS_SUCCEEDED(RunProcessDetached(argv))) {
      return NS_OK;
    }
  }
  QUrl url = QUrl::fromUserInput(QString::fromUtf8(
      PromiseFlatCString(aUriOrPath).get()));
  if (QDesktopServices::openUrl(url)) {
    return NS_OK;
  }
  // Last resort: try portal even outside sandbox.
  if (OpenWithXdgPortal(aUriOrPath)) {
    return NS_OK;
  }
  return NS_ERROR_FAILURE;
}

static nsresult LaunchDesktopEntry(const DesktopEntry& aEntry,
                                   const nsACString& aUriOrPath,
                                   bool aIsFilePath) {
  if (IsKdeDesktop() && CommandExists("kioclient") && !aEntry.path.IsEmpty()) {
    nsTArray<nsCString> argv;
    argv.AppendElement("kioclient"_ns);
    argv.AppendElement("exec"_ns);
    argv.AppendElement(aEntry.path);
    argv.AppendElement(nsCString(aUriOrPath));
    if (NS_SUCCEEDED(RunProcessDetached(argv))) {
      return NS_OK;
    }
  }
  return ExpandAndLaunchExec(aEntry.exec, aEntry.name, aEntry.icon, aEntry.path,
                             aUriOrPath, aIsFilePath);
}

// ---------------------------------------------------------------------------
// Handler app XPCOM wrappers
// ---------------------------------------------------------------------------

class nsDesktopHandlerApp final : public nsIGIOHandlerApp {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIHANDLERAPP
  NS_DECL_NSIGIOHANDLERAPP

  explicit nsDesktopHandlerApp(DesktopEntry aEntry)
      : mEntry(std::move(aEntry)) {}

 private:
  ~nsDesktopHandlerApp() = default;
  DesktopEntry mEntry;
};

NS_IMPL_ISUPPORTS(nsDesktopHandlerApp, nsIGIOHandlerApp, nsIHandlerApp)

NS_IMETHODIMP
nsDesktopHandlerApp::GetId(nsACString& aId) {
  aId = mEntry.id;
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopHandlerApp::GetName(nsAString& aName) {
  CopyUTF8toUTF16(mEntry.name, aName);
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopHandlerApp::SetName(const nsAString&) { return NS_OK; }

NS_IMETHODIMP
nsDesktopHandlerApp::GetDetailedDescription(nsAString& aDesc) {
  // Prefer Comment from .desktop when present; else empty.
  CopyUTF8toUTF16(mEntry.comment, aDesc);
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopHandlerApp::SetDetailedDescription(const nsAString&) {
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopHandlerApp::Equals(nsIHandlerApp* aHandlerApp, bool* aRet) {
  *aRet = false;
  nsCOMPtr<nsIGIOHandlerApp> other = do_QueryInterface(aHandlerApp);
  if (!other) {
    return NS_OK;
  }
  nsAutoCString theirId;
  other->GetId(theirId);
  *aRet = mEntry.id.Equals(theirId);
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopHandlerApp::LaunchFile(const nsACString& aFileName) {
  return LaunchDesktopEntry(mEntry, aFileName, /*aIsFilePath=*/true);
}

NS_IMETHODIMP
nsDesktopHandlerApp::LaunchWithURI(
    nsIURI* aUri, mozilla::dom::BrowsingContext* /*aBrowsingContext*/) {
  if (!aUri) {
    return NS_ERROR_INVALID_ARG;
  }
  nsAutoCString spec;
  nsresult rv = aUri->GetSpec(spec);
  NS_ENSURE_SUCCESS(rv, rv);
  return LaunchDesktopEntry(mEntry, spec, /*aIsFilePath=*/false);
}

NS_IMETHODIMP
nsDesktopHandlerApp::GetMozIconURL(nsACString& aRet) {
  // Preferences use moz-icon:// + id for nsIGIOHandlerApp.
  // Also surface theme icon name when available.
  if (!mEntry.icon.IsEmpty() && mEntry.icon[0] != '/') {
    aRet = mEntry.icon;
  } else {
    aRet = mEntry.id;
  }
  return NS_OK;
}

class nsDesktopMimeApp final : public nsIGIOMimeApp {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIHANDLERAPP
  NS_DECL_NSIGIOMIMEAPP

  explicit nsDesktopMimeApp(DesktopEntry aEntry) : mEntry(std::move(aEntry)) {}

  // Synthetic command-only app (no desktop file).
  nsDesktopMimeApp(const nsACString& aCommand, const nsACString& aName)
      : mSynthetic(true) {
    mEntry.exec = aCommand;
    mEntry.name = aName;
    mEntry.id = aCommand;
    mEntry.expectsUris = true;
  }

 private:
  ~nsDesktopMimeApp() = default;
  DesktopEntry mEntry;
  bool mSynthetic = false;
};

NS_IMPL_ISUPPORTS(nsDesktopMimeApp, nsIGIOMimeApp, nsIHandlerApp)

NS_IMETHODIMP
nsDesktopMimeApp::GetId(nsACString& aId) {
  aId = mEntry.id;
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::GetName(nsAString& aName) {
  CopyUTF8toUTF16(mEntry.name, aName);
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::SetName(const nsAString&) { return NS_OK; }

NS_IMETHODIMP
nsDesktopMimeApp::GetCommand(nsACString& aCommand) {
  aCommand = mEntry.exec;
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::GetExpectsURIs(int32_t* aExpects) {
  *aExpects = mEntry.expectsUris ? nsIGIOMimeApp::EXPECTS_URIS
                                 : nsIGIOMimeApp::EXPECTS_PATHS;
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::GetDetailedDescription(nsAString& aDesc) {
  CopyUTF8toUTF16(mEntry.comment, aDesc);
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::SetDetailedDescription(const nsAString&) { return NS_OK; }

NS_IMETHODIMP
nsDesktopMimeApp::Equals(nsIHandlerApp* aHandlerApp, bool* aRet) {
  *aRet = false;
  if (!aHandlerApp) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsILocalHandlerApp> local = do_QueryInterface(aHandlerApp);
  if (local) {
    nsAutoString theirName, thisName;
    GetName(thisName);
    local->GetName(theirName);
    *aRet = thisName.Equals(theirName);
    return NS_OK;
  }

  nsCOMPtr<nsIGIOMimeApp> other = do_QueryInterface(aHandlerApp);
  if (other) {
    nsAutoCString theirCmd;
    other->GetCommand(theirCmd);
    // Compare first token of Exec
    auto firstToken = [](const nsACString& cmd) {
      nsAutoCString c(cmd);
      int32_t sp = c.FindChar(' ');
      if (sp != kNotFound) {
        c.Truncate(sp);
      }
      return c;
    };
    *aRet = firstToken(mEntry.exec).Equals(firstToken(theirCmd));
    return NS_OK;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::LaunchWithURI(
    nsIURI* aUri, mozilla::dom::BrowsingContext* /*aBrowsingContext*/) {
  nsAutoCString spec;
  aUri->GetSpec(spec);
  if (mSynthetic) {
    return ExpandAndLaunchExec(mEntry.exec, mEntry.name, mEntry.icon,
                               ""_ns, spec, /*aIsFilePath=*/false);
  }
  return LaunchDesktopEntry(mEntry, spec, /*aIsFilePath=*/false);
}

class Utf8StringEnumerator final : public nsStringEnumeratorBase {
 public:
  Utf8StringEnumerator() = default;
  NS_DECL_ISUPPORTS
  NS_DECL_NSIUTF8STRINGENUMERATOR
  using nsStringEnumeratorBase::GetNext;
  nsTArray<nsCString> mStrings;
  uint32_t mIndex = 0;

 private:
  ~Utf8StringEnumerator() = default;
};

NS_IMPL_ISUPPORTS(Utf8StringEnumerator, nsIUTF8StringEnumerator,
                  nsIStringEnumerator)

NS_IMETHODIMP
Utf8StringEnumerator::HasMore(bool* aResult) {
  *aResult = mIndex < mStrings.Length();
  return NS_OK;
}

NS_IMETHODIMP
Utf8StringEnumerator::GetNext(nsACString& aResult) {
  if (mIndex >= mStrings.Length()) {
    return NS_ERROR_UNEXPECTED;
  }
  aResult = mStrings[mIndex++];
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::GetSupportedURISchemes(nsIUTF8StringEnumerator** aSchemes) {
  RefPtr<Utf8StringEnumerator> array = new Utf8StringEnumerator();
  for (const auto& mime : mEntry.mimeTypes) {
    if (StringBeginsWith(mime, "x-scheme-handler/"_ns)) {
      array->mStrings.AppendElement(Substring(mime, strlen("x-scheme-handler/")));
    }
  }
  array.forget(aSchemes);
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::SetAsDefaultForMimeType(const nsACString& aMimeType) {
  if (mEntry.id.IsEmpty() || mSynthetic) {
    return NS_ERROR_FAILURE;
  }
  nsTArray<nsCString> args;
  args.AppendElement("default"_ns);
  args.AppendElement(mEntry.id);
  args.AppendElement(nsCString(aMimeType));
  int exitStatus = -1;
  nsresult rv = RunProcessCapture("xdg-mime", args, nullptr, &exitStatus);
  if (NS_FAILED(rv) || exitStatus != 0) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::SetAsDefaultForFileExtensions(const nsACString& aExts) {
  // Map extensions via QMimeDatabase then set defaults.
  QMimeDatabase db;
  nsCString rest(aExts);
  while (!rest.IsEmpty()) {
    int32_t sp = rest.FindChar(' ');
    nsAutoCString ext;
    if (sp == kNotFound) {
      ext = rest;
      rest.Truncate();
    } else {
      ext = Substring(rest, 0, sp);
      rest = Substring(rest, sp + 1);
    }
    TrimInPlace(ext);
    if (ext.IsEmpty()) {
      continue;
    }
    QMimeType mt = db.mimeTypeForFile(
        QStringLiteral("file.") + QString::fromUtf8(ext.get()));
    if (!mt.isValid() || mt.name() == QStringLiteral("application/octet-stream")) {
      continue;
    }
    nsAutoCString mime;
    mime.Assign(mt.name().toUtf8().constData());
    nsresult rv = SetAsDefaultForMimeType(mime);
    if (NS_FAILED(rv)) {
      return rv;
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsDesktopMimeApp::SetAsDefaultForURIScheme(const nsACString& aURIScheme) {
  nsAutoCString mime("x-scheme-handler/");
  mime.Append(aURIScheme);
  return SetAsDefaultForMimeType(mime);
}

// ---------------------------------------------------------------------------
// nsUnixDesktopService
// ---------------------------------------------------------------------------

NS_IMPL_ISUPPORTS(nsUnixDesktopService, nsIGIOService)

static already_AddRefed<nsIHandlerApp> HandlerFromDesktopId(
    const nsACString& aId) {
  DesktopEntry entry;
  if (!FindDesktopById(aId, entry)) {
    // Still create a stub so callers know an association exists.
    entry.id = aId;
    entry.name = aId;
    entry.exec = "xdg-open"_ns;
    entry.expectsUris = true;
  }
  nsCOMPtr<nsIHandlerApp> app = new nsDesktopMimeApp(std::move(entry));
  return app.forget();
}

NS_IMETHODIMP
nsUnixDesktopService::GetMimeTypeFromExtension(const nsACString& aExtension,
                                               nsACString& aMimeType) {
  QMimeDatabase db;
  QString name = QStringLiteral("file.") +
                 QString::fromUtf8(PromiseFlatCString(aExtension).get());
  QMimeType mt = db.mimeTypeForFile(name);
  if (!mt.isValid()) {
    return NS_ERROR_FAILURE;
  }
  aMimeType.Assign(mt.name().toUtf8().constData());
  LOG("GetMimeTypeFromExtension(%s) -> %s",
      PromiseFlatCString(aExtension).get(),
      PromiseFlatCString(aMimeType).get());
  return NS_OK;
}

NS_IMETHODIMP
nsUnixDesktopService::GetAppForURIScheme(const nsACString& aURIScheme,
                                         nsIHandlerApp** aApp) {
  *aApp = nullptr;
  nsAutoCString mime("x-scheme-handler/");
  mime.Append(aURIScheme);
  LOG("GetAppForURIScheme %s", PromiseFlatCString(aURIScheme).get());

  // Sandbox / portal: cannot list host apps; ask portal SchemeSupported.
  if (PreferMimeHandlerPortal()) {
    if (aURIScheme.EqualsLiteral("http") || aURIScheme.EqualsLiteral("https") ||
        aURIScheme.EqualsLiteral("localhost") ||
        StringBeginsWith(aURIScheme, "127."_ns)) {
      // Loopback-ish schemes: fail like GIO portal path for localhost.
      if (aURIScheme.EqualsLiteral("localhost") ||
          StringBeginsWith(aURIScheme, "127."_ns)) {
        return NS_ERROR_FAILURE;
      }
    }
    if (!PortalSchemeSupported(aURIScheme)) {
      return NS_ERROR_FAILURE;
    }
    nsCOMPtr<nsIHandlerApp> portalApp = new nsPortalHandlerApp();
    portalApp.forget(aApp);
    return NS_OK;
  }

#ifdef MOZ_ENABLE_KF6_SERVICE
  {
    const QString scheme =
        QString::fromUtf8(PromiseFlatCString(aURIScheme).get());
    KService::Ptr service =
        KApplicationTrader::preferredService(QStringLiteral("x-scheme-handler/") +
                                             scheme);
    if (service) {
      DesktopEntry e;
      e.id.Assign(service->desktopEntryName().toUtf8().constData());
      if (!StringEndsWith(e.id, ".desktop"_ns)) {
        e.id.AppendLiteral(".desktop");
      }
      e.name.Assign(service->name().toUtf8().constData());
      e.exec.Assign(service->exec().toUtf8().constData());
      e.icon.Assign(service->icon().toUtf8().constData());
      e.path.Assign(service->entryPath().toUtf8().constData());
      e.expectsUris = service->exec().contains(QLatin1String("%u")) ||
                      service->exec().contains(QLatin1String("%U"));
      nsCOMPtr<nsIHandlerApp> app = new nsDesktopMimeApp(std::move(e));
      app.forget(aApp);
      return NS_OK;
    }
  }
#endif

  nsAutoCString desktopId;
  if (!ResolveDefaultDesktopIdForMime(mime, desktopId)) {
    return NS_ERROR_FAILURE;
  }
  nsCOMPtr<nsIHandlerApp> app = HandlerFromDesktopId(desktopId);
  app.forget(aApp);
  return NS_OK;
}

NS_IMETHODIMP
nsUnixDesktopService::GetAppsForURIScheme(const nsACString& aURIScheme,
                                          nsIMutableArray** aResult) {
  nsCOMPtr<nsIMutableArray> handlers =
      do_CreateInstance(NS_ARRAY_CONTRACTID);
  nsAutoCString mime("x-scheme-handler/");
  mime.Append(aURIScheme);

  LOG("GetAppsForURIScheme %s", mime.get());

  nsTArray<nsCString> orderedIds;
  nsAutoCString defaultId;
  if (ResolveDefaultDesktopIdForMime(mime, defaultId)) {
    orderedIds.AppendElement(defaultId);
  }
  LookupMimeappsAdded(mime, orderedIds);

  nsTArray<DesktopEntry> scanned;
  CollectDesktopEntriesForMime(mime, scanned);
  for (const auto& e : scanned) {
    if (!orderedIds.Contains(e.id)) {
      orderedIds.AppendElement(e.id);
    }
  }

#ifdef MOZ_ENABLE_KF6_SERVICE
  {
    auto services = KApplicationTrader::queryByMimeType(
        QString::fromUtf8(mime.get()));
    for (const KService::Ptr& service : services) {
      if (!service) {
        continue;
      }
      nsAutoCString id;
      id.Assign(service->desktopEntryName().toUtf8().constData());
      if (!StringEndsWith(id, ".desktop"_ns)) {
        id.AppendLiteral(".desktop");
      }
      if (!orderedIds.Contains(id)) {
        orderedIds.AppendElement(id);
      }
    }
  }
#endif

  nsTHashtable<nsCStringHashKey> added;
  for (const auto& id : orderedIds) {
    if (added.Contains(id)) {
      continue;
    }
    added.PutEntry(id);
    DesktopEntry entry;
    if (!FindDesktopById(id, entry)) {
      continue;
    }
    nsCOMPtr<nsIGIOMimeApp> app = new nsDesktopMimeApp(std::move(entry));
    handlers->AppendElement(app);
  }

  handlers.forget(aResult);
  return NS_OK;
}

NS_IMETHODIMP
nsUnixDesktopService::GetAppForMimeType(const nsACString& aMimeType,
                                        nsIHandlerApp** aApp) {
  *aApp = nullptr;
  LOG("GetAppForMimeType %s", PromiseFlatCString(aMimeType).get());

  if (aMimeType.EqualsLiteral("application/octet-stream")) {
    return NS_ERROR_NOT_AVAILABLE;
  }

#ifdef MOZ_ENABLE_KF6_SERVICE
  {
    KService::Ptr service = KApplicationTrader::preferredService(
        QString::fromUtf8(PromiseFlatCString(aMimeType).get()));
    if (service) {
      DesktopEntry e;
      e.id.Assign(service->desktopEntryName().toUtf8().constData());
      if (!StringEndsWith(e.id, ".desktop"_ns)) {
        e.id.AppendLiteral(".desktop");
      }
      e.name.Assign(service->name().toUtf8().constData());
      e.exec.Assign(service->exec().toUtf8().constData());
      e.icon.Assign(service->icon().toUtf8().constData());
      e.path.Assign(service->entryPath().toUtf8().constData());
      e.expectsUris = service->exec().contains(QLatin1String("%u")) ||
                      service->exec().contains(QLatin1String("%U"));
      nsCOMPtr<nsIHandlerApp> app = new nsDesktopMimeApp(std::move(e));
      app.forget(aApp);
      return NS_OK;
    }
  }
#endif

  nsAutoCString desktopId;
  if (!ResolveDefaultDesktopIdForMime(aMimeType, desktopId)) {
    return NS_ERROR_FAILURE;
  }
  nsCOMPtr<nsIHandlerApp> app = HandlerFromDesktopId(desktopId);
  app.forget(aApp);
  return NS_OK;
}

NS_IMETHODIMP
nsUnixDesktopService::GetDescriptionForMimeType(const nsACString& aMimeType,
                                                nsACString& aDescription) {
  QMimeDatabase db;
  QMimeType mt =
      db.mimeTypeForName(QString::fromUtf8(PromiseFlatCString(aMimeType).get()));
  if (!mt.isValid()) {
    return NS_ERROR_FAILURE;
  }
  aDescription.Assign(mt.comment().toUtf8().constData());
  if (aDescription.IsEmpty()) {
    aDescription.Assign(mt.name().toUtf8().constData());
  }
  return NS_OK;
}

NS_IMETHODIMP
nsUnixDesktopService::ShowURI(nsIURI* aURI) {
  nsAutoCString spec;
  MOZ_TRY(aURI->GetSpec(spec));
  LOG("ShowURI %s", spec.get());
  return OpenWithSystemDefault(spec);
}

NS_IMETHODIMP
nsUnixDesktopService::LaunchFile(const nsACString& aPath) {
  LOG("LaunchFile %s", PromiseFlatCString(aPath).get());
  // Prefer file path for kioclient/xdg-open
  return OpenWithSystemDefault(aPath);
}

NS_IMETHODIMP
nsUnixDesktopService::RevealFile(nsIFile* aFile) {
  nsAutoCString path;
  MOZ_TRY(aFile->GetNativePath(path));
  LOG("RevealFile %s", path.get());

  bool isDir = false;
  if (NS_SUCCEEDED(aFile->IsDirectory(&isDir)) && isDir) {
    return OpenWithSystemDefault(path);
  }

  // org.freedesktop.FileManager1.ShowItems
  nsAutoCString uri("file://");
  // Use QUrl for proper encoding
  QUrl fileUrl = QUrl::fromLocalFile(QString::fromUtf8(path.get()));
  QString uriStr = fileUrl.toString(QUrl::FullyEncoded);

  QDBusMessage msg = QDBusMessage::createMethodCall(
      QStringLiteral("org.freedesktop.FileManager1"),
      QStringLiteral("/org/freedesktop/FileManager1"),
      QStringLiteral("org.freedesktop.FileManager1"),
      QStringLiteral("ShowItems"));
  msg << QStringList{uriStr} << QString();
  QDBusMessage reply = QDBusConnection::sessionBus().call(msg);
  if (reply.type() != QDBusMessage::ErrorMessage) {
    return NS_OK;
  }
  LOG("FileManager1 failed: %s", reply.errorMessage().toUtf8().constData());

  // Fallbacks
  if (IsKdeDesktop() && CommandExists("kioclient")) {
    nsTArray<nsCString> argv;
    argv.AppendElement("kioclient"_ns);
    // `kioclient exec` with highlight not portable; open parent dir.
  }
  nsCOMPtr<nsIFile> parent;
  if (NS_SUCCEEDED(aFile->GetParent(getter_AddRefs(parent))) && parent) {
    nsAutoCString parentPath;
    if (NS_SUCCEEDED(parent->GetNativePath(parentPath))) {
      return OpenWithSystemDefault(parentPath);
    }
  }
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsUnixDesktopService::GetIsRunningUnderFlatpak(bool* aResult) {
  *aResult = PR_GetEnv("FLATPAK_ID") != nullptr;
  return NS_OK;
}

NS_IMETHODIMP
nsUnixDesktopService::GetIsRunningUnderSnap(bool* aResult) {
  const char* snap = PR_GetEnv("SNAP");
  *aResult = snap && *snap;
  return NS_OK;
}

NS_IMETHODIMP
nsUnixDesktopService::CreateHandlerAppFromAppId(const char* aAppId,
                                                nsIGIOHandlerApp** aResult) {
  LOG("CreateHandlerAppFromAppId %s", aAppId);
  DesktopEntry entry;
  if (!FindDesktopById(nsDependentCString(aAppId), entry)) {
    return NS_ERROR_FAILURE;
  }
  nsCOMPtr<nsIGIOHandlerApp> app = new nsDesktopHandlerApp(std::move(entry));
  app.forget(aResult);
  return NS_OK;
}

NS_IMETHODIMP
nsUnixDesktopService::CreateAppFromCommand(const nsACString& aCmd,
                                           const nsACString& aAppName,
                                           nsIGIOMimeApp** aAppInfo) {
  *aAppInfo = nullptr;
  LOG("CreateAppFromCommand %s : %s", PromiseFlatCString(aAppName).get(),
      PromiseFlatCString(aCmd).get());

  // Strip arguments; keep binary.
  nsAutoCString cmd(aCmd);
  // Take first token
  nsAutoCString binary;
  {
    nsAutoCString tmp(cmd);
    // rough: skip leading
    TrimInPlace(tmp);
    if (tmp.IsEmpty()) {
      return NS_ERROR_FAILURE;
    }
    if (tmp[0] == '"' || tmp[0] == '\'') {
      char q = tmp[0];
      int32_t end = tmp.FindChar(q, 1);
      if (end == kNotFound) {
        return NS_ERROR_FAILURE;
      }
      binary = Substring(tmp, 1, end - 1);
    } else {
      int32_t sp = tmp.FindChar(' ');
      if (sp == kNotFound) {
        binary = tmp;
      } else {
        binary.Assign(Substring(tmp, 0, sp));
      }
    }
  }

  QString found =
      QStandardPaths::findExecutable(QString::fromUtf8(binary.get()));
  if (found.isEmpty() && binary[0] == '/') {
    if (!QFileInfo::exists(QString::fromUtf8(binary.get()))) {
      return NS_ERROR_FILE_NOT_FOUND;
    }
  } else if (found.isEmpty()) {
    return NS_ERROR_FILE_NOT_FOUND;
  }

  nsAutoCString exec(binary);
  exec.AppendLiteral(" %u");
  RefPtr<nsDesktopMimeApp> app = new nsDesktopMimeApp(exec, aAppName);
  app.forget(aAppInfo);
  return NS_OK;
}

NS_IMETHODIMP
nsUnixDesktopService::FindAppFromCommand(const nsACString& aCmd,
                                         nsIGIOMimeApp** aAppInfo) {
  *aAppInfo = nullptr;
  LOG("FindAppFromCommand %s", PromiseFlatCString(aCmd).get());

  nsAutoCString want(aCmd);
  // Resolve to absolute if needed
  if (want[0] != '/') {
    QString found =
        QStandardPaths::findExecutable(QString::fromUtf8(want.get()));
    if (!found.isEmpty()) {
      want.Assign(found.toUtf8().constData());
    }
  }

  nsTArray<nsCString> dirs;
  CollectApplicationDirs(dirs);
  for (const auto& dir : dirs) {
    QDirIterator it(QString::fromUtf8(dir.get()),
                    QStringList() << QStringLiteral("*.desktop"), QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      QFileInfo fi = it.fileInfo();
      nsAutoCString id;
      id.Assign(fi.fileName().toUtf8().constData());
      nsAutoCString path;
      path.Assign(fi.absoluteFilePath().toUtf8().constData());
      DesktopEntry entry;
      if (!ParseDesktopFile(path, id, entry)) {
        continue;
      }
      // First token of Exec
      nsAutoCString execBin(entry.exec);
      TrimInPlace(execBin);
      if (execBin.IsEmpty()) {
        continue;
      }
      nsAutoCString bin;
      if (execBin[0] == '"' || execBin[0] == '\'') {
        char q = execBin[0];
        int32_t end = execBin.FindChar(q, 1);
        if (end != kNotFound) {
          bin = Substring(execBin, 1, end - 1);
        }
      } else {
        int32_t sp = execBin.FindChar(' ');
        if (sp == kNotFound) {
          bin = execBin;
        } else {
          bin.Assign(Substring(execBin, 0, sp));
        }
      }
      if (bin.IsEmpty()) {
        continue;
      }
      nsAutoCString resolved(bin);
      if (bin[0] != '/') {
        QString f = QStandardPaths::findExecutable(QString::fromUtf8(bin.get()));
        if (!f.isEmpty()) {
          resolved.Assign(f.toUtf8().constData());
        }
      }
      if (resolved.Equals(want) || bin.Equals(aCmd)) {
        RefPtr<nsDesktopMimeApp> app = new nsDesktopMimeApp(std::move(entry));
        app.forget(aAppInfo);
        return NS_OK;
      }
    }
  }
  return NS_ERROR_NOT_AVAILABLE;
}
