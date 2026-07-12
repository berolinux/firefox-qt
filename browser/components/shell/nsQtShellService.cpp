/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsQtShellService.h"

#include "imgIContainer.h"
#include "imgIRequest.h"
#include "mozilla/dom/Element.h"
#include "mozilla/gfx/2D.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsIImageLoadingContent.h"
#include "nsIPrefBranch.h"
#include "nsIPrefService.h"
#include "mozilla/Components.h"
#include "nsIStringBundle.h"
#include "nsReadableUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsShellService.h"
#include "nsString.h"
#include "prenv.h"

#include <fcntl.h>
#include <spawn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#pragma GCC visibility push(default)
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QPalette>
#include <QStandardPaths>
#include <QString>
#pragma GCC visibility pop

#include "nsPrintfCString.h"

// From glibc / POSIX: environ for posix_spawnp.
extern char** environ;

using mozilla::gfx::DataSourceSurface;
using mozilla::gfx::SourceSurface;
using mozilla::gfx::SurfaceFormat;

NS_IMPL_ISUPPORTS(nsQtShellService, nsIUnixShellService, nsIShellService,
                  nsIToolkitShellService)

// Last solid color applied via SetDesktopBackgroundColor (Plasma has no
// stable public API to read wallpaper Color back).
static bool sHaveDesktopBgColor = false;
static uint32_t sDesktopBgColor = 0;

static void AppendLiteral(nsTArray<nsCString>& aArgs, const char* aLit) {
  aArgs.AppendElement()->Assign(aLit);
}

/* static */
nsresult nsQtShellService::RunProcess(const char* aExecutable,
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

/* static */
nsresult nsQtShellService::RunXdgSettings(const nsTArray<nsCString>& aArgs,
                                          nsACString* aStdout,
                                          int* aExitStatus) {
  return RunProcess("xdg-settings", aArgs, aStdout, aExitStatus);
}

/* static */
bool nsQtShellService::IsPlasmaSession() {
  if (const char* desktop = PR_GetEnv("XDG_CURRENT_DESKTOP")) {
    if (strstr(desktop, "KDE") || strstr(desktop, "Plasma")) {
      return true;
    }
  }
  if (const char* session = PR_GetEnv("DESKTOP_SESSION")) {
    if (strstr(session, "plasma") || strstr(session, "kde")) {
      return true;
    }
  }
  return false;
}

/* static */
bool nsQtShellService::HasPlasmaWallpaperTool() {
  return !QStandardPaths::findExecutable(
              QStringLiteral("plasma-apply-wallpaperimage"))
              .isEmpty();
}

/* static */
bool nsQtShellService::ResolveDesktopId(nsACString& aDesktopId) {
  if (const char* envDesktop = PR_GetEnv("MOZ_DESKTOP_FILE_NAME")) {
    aDesktopId.Assign(envDesktop);
    if (!StringEndsWith(aDesktopId, ".desktop"_ns)) {
      aDesktopId.AppendLiteral(".desktop");
    }
    return true;
  }

  static const char* kCandidates[] = {
      MOZ_APP_NAME ".desktop",
      "org.mozilla." MOZ_APP_NAME ".desktop",
      MOZ_APP_NAME "-esr.desktop",
  };

  for (const char* candidate : kCandidates) {
    nsTArray<nsCString> args;
    AppendLiteral(args, "check");
    AppendLiteral(args, "default-web-browser");
    nsCString cand;
    cand.Assign(candidate);
    args.AppendElement(cand);

    nsAutoCString output;
    int exitStatus = -1;
    if (NS_SUCCEEDED(RunXdgSettings(args, &output, &exitStatus))) {
      if (StringBeginsWith(output, "yes"_ns) ||
          StringBeginsWith(output, "no"_ns)) {
        aDesktopId.Assign(candidate);
        return true;
      }
    }
  }

  aDesktopId.AssignLiteral(MOZ_APP_NAME ".desktop");
  return true;
}

nsresult nsQtShellService::Init() {
  if (!ResolveDesktopId(mDesktopId)) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  return NS_OK;
}

bool nsQtShellService::DesktopIdMatchesHandler(
    const nsACString& aHandlerId) const {
  if (aHandlerId.IsEmpty()) {
    return false;
  }
  if (mDesktopId.Equals(aHandlerId)) {
    return true;
  }
  // Compare basenames without .desktop
  auto strip = [](const nsACString& id) {
    nsAutoCString s(id);
    if (StringEndsWith(s, ".desktop"_ns)) {
      s.Truncate(s.Length() - strlen(".desktop"));
    }
    return s;
  };
  return strip(mDesktopId).Equals(strip(aHandlerId));
}

NS_IMETHODIMP
nsQtShellService::IsDefaultBrowser(bool /*aForAllTypes*/,
                                   bool* aIsDefaultBrowser) {
  NS_ENSURE_ARG_POINTER(aIsDefaultBrowser);
  *aIsDefaultBrowser = false;

  nsTArray<nsCString> args;
  AppendLiteral(args, "check");
  AppendLiteral(args, "default-web-browser");
  args.AppendElement(mDesktopId);

  nsAutoCString output;
  int exitStatus = -1;
  nsresult rv = RunXdgSettings(args, &output, &exitStatus);
  if (NS_FAILED(rv) || exitStatus != 0) {
    return NS_OK;
  }
  *aIsDefaultBrowser = StringBeginsWith(output, "yes"_ns);
  return NS_OK;
}

NS_IMETHODIMP
nsQtShellService::SetDefaultBrowser(bool aForAllUsers) {
#ifdef DEBUG
  if (aForAllUsers) {
    NS_WARNING(
        "Setting the default browser for all users is not supported on Linux");
  }
#else
  (void)aForAllUsers;
#endif

  nsTArray<nsCString> args;
  AppendLiteral(args, "set");
  AppendLiteral(args, "default-web-browser");
  args.AppendElement(mDesktopId);

  int exitStatus = -1;
  nsresult rv = RunXdgSettings(args, nullptr, &exitStatus);
  if (NS_FAILED(rv) || exitStatus != 0) {
    return NS_ERROR_FAILURE;
  }

  // Web MIME + schemes (matches KDE System Settings "Web Browser").
  {
    nsTArray<nsCString> mimeArgs;
    AppendLiteral(mimeArgs, "default");
    mimeArgs.AppendElement(mDesktopId);
    AppendLiteral(mimeArgs, "text/html");
    AppendLiteral(mimeArgs, "application/xhtml+xml");
    AppendLiteral(mimeArgs, "x-scheme-handler/http");
    AppendLiteral(mimeArgs, "x-scheme-handler/https");
    int mimeStatus = -1;
    (void)RunProcess("xdg-mime", mimeArgs, nullptr, &mimeStatus);
  }

  // Also claim common browser schemes via xdg-settings when supported.
  for (const char* scheme : {"http", "https"}) {
    nsTArray<nsCString> schemeArgs;
    AppendLiteral(schemeArgs, "set");
    AppendLiteral(schemeArgs, "default-url-scheme-handler");
    schemeArgs.AppendElement()->Assign(scheme);
    schemeArgs.AppendElement(mDesktopId);
    int st = -1;
    (void)RunXdgSettings(schemeArgs, nullptr, &st);
  }

  // Plasma: ensure BrowserApplication matches (xdg-settings usually does this).
  if (IsPlasmaSession()) {
    for (const char* bin : {"kwriteconfig6", "kwriteconfig5"}) {
      nsTArray<nsCString> kargs;
      AppendLiteral(kargs, "--file");
      AppendLiteral(kargs, "kdeglobals");
      AppendLiteral(kargs, "--group");
      AppendLiteral(kargs, "General");
      AppendLiteral(kargs, "--key");
      AppendLiteral(kargs, "BrowserApplication");
      kargs.AppendElement(mDesktopId);
      int st = -1;
      if (NS_SUCCEEDED(RunProcess(bin, kargs, nullptr, &st)) && st == 0) {
        break;
      }
    }
  }

  nsCOMPtr<nsIPrefBranch> prefs(do_GetService(NS_PREFSERVICE_CONTRACTID));
  if (prefs) {
    (void)prefs->SetBoolPref(PREF_CHECKDEFAULTBROWSER, true);
    (void)prefs->SetIntPref(PREF_DEFAULTBROWSERCHECKCOUNT, 0);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsQtShellService::GetCanSetDesktopBackground(bool* aResult) {
  NS_ENSURE_ARG_POINTER(aResult);
  *aResult = IsPlasmaSession() && HasPlasmaWallpaperTool();
  return NS_OK;
}

/* static */
nsresult nsQtShellService::WriteElementImageToPng(
    mozilla::dom::Element* aElement, const nsACString& aPath) {
  nsCOMPtr<nsIImageLoadingContent> imageContent = do_QueryInterface(aElement);
  if (!imageContent) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<imgIRequest> request;
  imageContent->GetRequest(nsIImageLoadingContent::CURRENT_REQUEST,
                           getter_AddRefs(request));
  if (!request) {
    return NS_ERROR_FAILURE;
  }
  nsCOMPtr<imgIContainer> container;
  request->GetImage(getter_AddRefs(container));
  if (!container) {
    return NS_ERROR_FAILURE;
  }

  const uint32_t flags =
      imgIContainer::FLAG_SYNC_DECODE | imgIContainer::FLAG_ASYNC_NOTIFY;
  RefPtr<SourceSurface> surface =
      container->GetFrame(imgIContainer::FRAME_CURRENT, flags);
  if (!surface) {
    surface =
        container->GetFrame(imgIContainer::FRAME_CURRENT, imgIContainer::FLAG_NONE);
  }
  if (!surface) {
    return NS_ERROR_FAILURE;
  }

  RefPtr<DataSourceSurface> data = surface->GetDataSurface();
  if (!data) {
    return NS_ERROR_FAILURE;
  }

  DataSourceSurface::ScopedMap map(data, DataSourceSurface::READ);
  if (!map.IsMapped()) {
    return NS_ERROR_FAILURE;
  }

  const int width = data->GetSize().width;
  const int height = data->GetSize().height;
  if (width <= 0 || height <= 0) {
    return NS_ERROR_FAILURE;
  }

  // Convert to RGBA8888 for QImage.
  QImage image(width, height, QImage::Format_RGBA8888);
  if (image.isNull()) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  const SurfaceFormat fmt = data->GetFormat();
  const uint8_t* src = map.GetData();
  const int32_t srcStride = map.GetStride();

  for (int y = 0; y < height; ++y) {
    const uint8_t* row = src + y * srcStride;
    uint8_t* dst = image.scanLine(y);
    for (int x = 0; x < width; ++x) {
      uint8_t r = 0, g = 0, b = 0, a = 255;
      switch (fmt) {
        case SurfaceFormat::B8G8R8A8:
        case SurfaceFormat::B8G8R8X8: {
          b = row[x * 4 + 0];
          g = row[x * 4 + 1];
          r = row[x * 4 + 2];
          a = row[x * 4 + 3];
          break;
        }
        case SurfaceFormat::R8G8B8A8:
        case SurfaceFormat::R8G8B8X8: {
          r = row[x * 4 + 0];
          g = row[x * 4 + 1];
          b = row[x * 4 + 2];
          a = row[x * 4 + 3];
          break;
        }
        default: {
          // Best-effort BGRA (covers OS_RGBA / A8R8G8B8_UINT32 on LE).
          b = row[x * 4 + 0];
          g = row[x * 4 + 1];
          r = row[x * 4 + 2];
          a = row[x * 4 + 3];
          break;
        }
      }
      dst[x * 4 + 0] = r;
      dst[x * 4 + 1] = g;
      dst[x * 4 + 2] = b;
      dst[x * 4 + 3] = a;
    }
  }

  if (!image.save(QString::fromUtf8(PromiseFlatCString(aPath).get()), "PNG")) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsQtShellService::SetDesktopBackground(mozilla::dom::Element* aElement,
                                       int32_t aPosition,
                                       const nsACString& /*aImageName*/) {
  if (!IsPlasmaSession() || !HasPlasmaWallpaperTool()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // Write wallpaper under $HOME (same pattern as GNOME shell service).
  const char* home = PR_GetEnv("HOME");
  if (!home || !*home) {
    return NS_ERROR_FAILURE;
  }

  nsAutoString brandName;
  if (nsCOMPtr<nsIStringBundleService> bundleService =
          mozilla::components::StringBundle::Service()) {
    nsCOMPtr<nsIStringBundle> brandBundle;
    if (NS_SUCCEEDED(bundleService->CreateBundle(
            SHELL_BRAND_PROPERTIES_URI, getter_AddRefs(brandBundle))) &&
        brandBundle) {
      brandBundle->GetStringFromName("brandShortName", brandName);
    }
  }
  if (brandName.IsEmpty()) {
    brandName.AssignLiteral("Firefox");
  }

  nsAutoCString filePath(home);
  filePath.Append('/');
  filePath.Append(NS_ConvertUTF16toUTF8(brandName));
  filePath.AppendLiteral("_wallpaper.png");

  nsresult rv = WriteElementImageToPng(aElement, filePath);
  NS_ENSURE_SUCCESS(rv, rv);

  // Map nsIShellService position flags to plasma-apply-wallpaperimage fill modes.
  const char* fillMode = "preserveAspectCrop";
  switch (aPosition) {
    case BACKGROUND_TILE:
      fillMode = "tile";
      break;
    case BACKGROUND_STRETCH:
      fillMode = "stretch";
      break;
    case BACKGROUND_FILL:
      fillMode = "preserveAspectCrop";
      break;
    case BACKGROUND_FIT:
      fillMode = "preserveAspectFit";
      break;
    case BACKGROUND_SPAN:
      fillMode = "preserveAspectCrop";
      break;
    case BACKGROUND_CENTER:
    default:
      fillMode = "pad";
      break;
  }

  nsTArray<nsCString> args;
  AppendLiteral(args, "-f");
  args.AppendElement()->Assign(fillMode);
  args.AppendElement(filePath);

  int exitStatus = -1;
  rv = RunProcess("plasma-apply-wallpaperimage", args, nullptr, &exitStatus);
  if (NS_FAILED(rv) || exitStatus != 0) {
    // Retry without fill mode for older tools.
    nsTArray<nsCString> plain;
    plain.AppendElement(filePath);
    exitStatus = -1;
    rv = RunProcess("plasma-apply-wallpaperimage", plain, nullptr, &exitStatus);
    if (NS_FAILED(rv) || exitStatus != 0) {
      return NS_ERROR_FAILURE;
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsQtShellService::GetDesktopBackgroundColor(uint32_t* aColor) {
  NS_ENSURE_ARG_POINTER(aColor);
  if (sHaveDesktopBgColor) {
    *aColor = sDesktopBgColor;
    return NS_OK;
  }
  // Best-effort: session palette window color (Plasma theme / system UI).
  // Solid wallpaper Color is containment-specific and not a stable public API.
#pragma GCC visibility push(default)
  {
    QColor c = QGuiApplication::palette().color(QPalette::Window);
    *aColor = (uint32_t(c.red()) << 16) | (uint32_t(c.green()) << 8) |
              uint32_t(c.blue());
  }
#pragma GCC visibility pop
  return NS_OK;
}

NS_IMETHODIMP
nsQtShellService::SetDesktopBackgroundColor(uint32_t aColor) {
  if (!IsPlasmaSession()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  // Apply solid color wallpaper via PlasmaShell JS (org.kde.color plugin).
  const uint8_t r = (aColor >> 16) & 0xff;
  const uint8_t g = (aColor >> 8) & 0xff;
  const uint8_t b = aColor & 0xff;
  nsPrintfCString hex("#%02x%02x%02x", r, g, b);
  nsPrintfCString script(
      "var allDesktops = desktops();"
      "for (var i = 0; i < allDesktops.length; i++) {"
      "  var d = allDesktops[i];"
      "  d.wallpaperPlugin = 'org.kde.color';"
      "  d.currentConfigGroup = Array('Wallpaper', 'org.kde.color', 'General');"
      "  d.writeConfig('Color', '%s');"
      "}",
      hex.get());
  nsTArray<nsCString> args;
  AppendLiteral(args, "org.kde.plasmashell");
  AppendLiteral(args, "/PlasmaShell");
  AppendLiteral(args, "org.kde.PlasmaShell.evaluateScript");
  args.AppendElement(script);
  int exitStatus = -1;
  nsresult rv = RunProcess("qdbus", args, nullptr, &exitStatus);
  if (NS_FAILED(rv) || exitStatus != 0) {
    // Plasma 6 often uses qdbus6 / qdbus-qt6.
    bool ok = false;
    for (const char* bin : {"qdbus6", "qdbus-qt6"}) {
      rv = RunProcess(bin, args, nullptr, &exitStatus);
      if (NS_SUCCEEDED(rv) && exitStatus == 0) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      return NS_ERROR_FAILURE;
    }
  }
  sDesktopBgColor = aColor & 0x00ffffff;
  sHaveDesktopBgColor = true;
  return NS_OK;
}

NS_IMETHODIMP
nsQtShellService::IsDefaultForScheme(const nsACString& aScheme,
                                     bool* aIsDefault) {
  NS_ENSURE_ARG_POINTER(aIsDefault);
  *aIsDefault = false;

  nsAutoCString scheme(aScheme);
  // xdg-settings check default-url-scheme-handler SCHEME DESKTOP
  {
    nsTArray<nsCString> args;
    AppendLiteral(args, "check");
    AppendLiteral(args, "default-url-scheme-handler");
    args.AppendElement(scheme);
    args.AppendElement(mDesktopId);
    nsAutoCString output;
    int exitStatus = -1;
    if (NS_SUCCEEDED(RunXdgSettings(args, &output, &exitStatus)) &&
        exitStatus == 0 && StringBeginsWith(output, "yes"_ns)) {
      *aIsDefault = true;
      return NS_OK;
    }
  }

  // Fallback: xdg-mime query default x-scheme-handler/SCHEME
  {
    nsAutoCString mime("x-scheme-handler/");
    mime.Append(scheme);
    nsTArray<nsCString> args;
    AppendLiteral(args, "query");
    AppendLiteral(args, "default");
    args.AppendElement(mime);
    nsAutoCString output;
    int exitStatus = -1;
    if (NS_SUCCEEDED(RunProcess("xdg-mime", args, &output, &exitStatus)) &&
        exitStatus == 0) {
      output.Trim(" \t\r\n", true, true);
      *aIsDefault = DesktopIdMatchesHandler(output);
      return NS_OK;
    }
  }

  return NS_OK;
}
