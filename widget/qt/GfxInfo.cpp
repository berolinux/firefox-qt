/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GfxInfo.h"

#include "MediaCodecsSupport.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/Maybe.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/StaticPtr.h"
#include "nsAppRunner.h"
#include "nsCRTGlue.h"
#include "nsIFile.h"
#include "nsPrintfCString.h"
#include "nsString.h"
#include "prenv.h"

#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <QGuiApplication>

namespace mozilla::widget {

#ifdef DEBUG
NS_IMPL_ISUPPORTS_INHERITED(GfxInfo, GfxInfoBase, nsIGfxInfoDebug)
#endif

static StaticAutoPtr<nsTArray<RefPtr<GfxDriverInfo>>> sDriverInfo;

// Match vaapitest.cpp bit layout.
constexpr int CODEC_HW_DEC_H264 = 1 << 4;
constexpr int CODEC_HW_ENC_H264 = 1 << 5;
constexpr int CODEC_HW_DEC_VP8 = 1 << 6;
constexpr int CODEC_HW_ENC_VP8 = 1 << 7;
constexpr int CODEC_HW_DEC_VP9 = 1 << 8;
constexpr int CODEC_HW_ENC_VP9 = 1 << 9;
constexpr int CODEC_HW_DEC_AV1 = 1 << 10;
constexpr int CODEC_HW_ENC_AV1 = 1 << 11;
constexpr int CODEC_HW_DEC_HEVC = 1 << 12;
constexpr int CODEC_HW_ENC_HEVC = 1 << 13;

#define VAAPI_TEST_TIMEOUT_MS 2000
#define VAAPI_PROBE_BINARY u"vaapitest"_ns

nsresult GfxInfo::Init() {
  DiscoverDrmRenderDevice();
  return GfxInfoBase::Init();
}

void GfxInfo::DiscoverDrmRenderDevice() {
  if (!mDrmRenderDevice.IsEmpty()) {
    return;
  }
  if (const char* env = PR_GetEnv("MOZ_DRM_DEVICE")) {
    if (*env) {
      mDrmRenderDevice.Assign(env);
      return;
    }
  }
  // Prefer the first accessible DRM render node (same order as common stacks).
  for (int i = 128; i < 192; ++i) {
    nsPrintfCString path("/dev/dri/renderD%d", i);
    int fd = open(path.get(), O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
      close(fd);
      mDrmRenderDevice = path;
      return;
    }
  }
}

// Spawn binDir/vaapitest with args; return pid and set *aOutPipe to stdout.
static pid_t SpawnProbe(const nsAString& aBinary, const char** aArgs,
                        int* aOutPipe) {
  nsCOMPtr<nsIFile> appFile;
  if (NS_FAILED(XRE_GetBinaryPath(getter_AddRefs(appFile)))) {
    return 0;
  }
  nsCOMPtr<nsIFile> exePath;
  if (NS_FAILED(appFile->GetParent(getter_AddRefs(exePath)))) {
    return 0;
  }
  exePath->Append(aBinary);

  int pipes[2];
  if (pipe(pipes) != 0) {
    return 0;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipes[0]);
    close(pipes[1]);
    return 0;
  }
  if (pid == 0) {
    // Child
    close(pipes[0]);
    dup2(pipes[1], STDOUT_FILENO);
    close(pipes[1]);
    // Close other fds? leave open is fine for short probe.
    char* argv[10];
    argv[0] = const_cast<char*>(exePath->NativePath().get());
    int i = 0;
    for (; aArgs[i] && i < 8; ++i) {
      argv[i + 1] = const_cast<char*>(aArgs[i]);
    }
    argv[i + 1] = nullptr;
    execv(argv[0], argv);
    _exit(127);
  }

  close(pipes[1]);
  *aOutPipe = pipes[0];
  return pid;
}

static bool ReadProbeOutput(pid_t aPid, int aPipe, int aTimeoutMs,
                            nsACString& aOut) {
  aOut.Truncate();
  std::string buf;
  char tmp[512];
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(aTimeoutMs);

  while (true) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      kill(aPid, SIGKILL);
      break;
    }
    int remaining = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());

    pollfd pfd{aPipe, POLLIN, 0};
    int pr = poll(&pfd, 1, remaining);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (pr == 0) {
      kill(aPid, SIGKILL);
      break;
    }
    if (pfd.revents & (POLLIN | POLLHUP)) {
      ssize_t n = read(aPipe, tmp, sizeof(tmp));
      if (n > 0) {
        buf.append(tmp, n);
      } else {
        break;
      }
    } else {
      break;
    }
  }
  close(aPipe);

  int status = 0;
  while (waitpid(aPid, &status, 0) < 0 && errno == EINTR) {
  }

  aOut.Assign(buf.data(), buf.size());
  return !aOut.IsEmpty();
}

void GfxInfo::GetDataVAAPI() {
  if (mIsVAAPISupported.isSome()) {
    return;
  }
  mIsVAAPISupported = Some(false);
  mVAAPISupportedCodecs = 0;

#ifdef MOZ_ENABLE_VAAPI
  DiscoverDrmRenderDevice();
  if (mDrmRenderDevice.IsEmpty()) {
    return;
  }

  const char* args[] = {"-d", mDrmRenderDevice.get(), nullptr};
  int pipeFd = -1;
  pid_t pid = SpawnProbe(VAAPI_PROBE_BINARY, args, &pipeFd);
  if (!pid) {
    gfxCriticalNote << "vaapitest: failed to spawn\n";
    return;
  }

  nsCString data;
  if (!ReadProbeOutput(pid, pipeFd, VAAPI_TEST_TIMEOUT_MS, data)) {
    gfxCriticalNote << "vaapitest: no output\n";
    return;
  }

  char* bufptr = data.BeginWriting();
  char* line;
  while ((line = NS_strtok("\n", &bufptr))) {
    if (!strcmp(line, "VAAPI_SUPPORTED")) {
      line = NS_strtok("\n", &bufptr);
      if (!line) {
        return;
      }
      mIsVAAPISupported = Some(!strcmp(line, "TRUE"));
    } else if (!strcmp(line, "VAAPI_HWCODECS")) {
      line = NS_strtok("\n", &bufptr);
      if (!line) {
        return;
      }
      std::istringstream(line) >> mVAAPISupportedCodecs;
#  define VAAPI_CODEC_CHECK(name)                           \
    if (mVAAPISupportedCodecs & CODEC_HW_DEC_##name) {      \
      media::MCSInfo::AddSupport(                           \
          media::MediaCodecsSupport::name##HardwareDecode); \
    }                                                       \
    if (mVAAPISupportedCodecs & CODEC_HW_ENC_##name) {      \
      media::MCSInfo::AddSupport(                           \
          media::MediaCodecsSupport::name##HardwareEncode); \
    }
      VAAPI_CODEC_CHECK(H264)
      VAAPI_CODEC_CHECK(VP8)
      VAAPI_CODEC_CHECK(VP9)
      VAAPI_CODEC_CHECK(AV1)
      VAAPI_CODEC_CHECK(HEVC)
#  undef VAAPI_CODEC_CHECK
    } else if (!strcmp(line, "WARNING") || !strcmp(line, "ERROR")) {
      line = NS_strtok("\n", &bufptr);
      if (line) {
        gfxCriticalNote << "vaapitest: " << line << "\n";
      }
      return;
    }
  }
#endif
}

NS_IMETHODIMP GfxInfo::GetDWriteEnabled(bool* aVal) {
  *aVal = false;
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetDWriteVersion(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetEmbeddedInFirefoxReality(bool* aVal) {
  *aVal = false;
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetHasBattery(bool* aVal) {
  *aVal = false;
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetCleartypeParameters(nsAString& aVal) {
  aVal.Truncate();
  return NS_ERROR_FAILURE;
}
NS_IMETHODIMP GfxInfo::GetWindowProtocol(nsAString& aVal) {
  if (QGuiApplication::platformName().contains(QLatin1String("wayland"),
                                               Qt::CaseInsensitive)) {
    aVal.AssignLiteral("wayland");
  } else {
    aVal.AssignLiteral("x11");
  }
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetTestType(nsAString& aVal) {
  // No glxtest; report EGL so EGL-based features are not blocked on test type.
  aVal.AssignLiteral("EGL");
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDescription(nsAString& aVal) {
  aVal.AssignLiteral("Qt");
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDriver(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterVendorID(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDeviceID(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterSubsysID(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterRAM(uint32_t* aVal) {
  *aVal = 0;
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDriverVendor(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDriverVersion(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDriverDate(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDescription2(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDriver2(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterVendorID2(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDeviceID2(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterSubsysID2(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterRAM2(uint32_t* aVal) {
  *aVal = 0;
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDriverVendor2(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDriverVersion2(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetAdapterDriverDate2(nsAString& aVal) {
  aVal.Truncate();
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetIsGPU2Active(bool* aVal) {
  *aVal = false;
  return NS_OK;
}
NS_IMETHODIMP GfxInfo::GetDrmRenderDevice(nsACString& aVal) {
  DiscoverDrmRenderDevice();
  aVal.Assign(mDrmRenderDevice);
  return NS_OK;
}

const nsTArray<RefPtr<GfxDriverInfo>>& GfxInfo::GetGfxDriverInfo() {
  if (!sDriverInfo) {
    sDriverInfo = new nsTArray<RefPtr<GfxDriverInfo>>();
  }
  return *sDriverInfo;
}

nsresult GfxInfo::GetFeatureStatusImpl(
    int32_t aFeature, int32_t* aStatus, nsAString& aSuggestedDriverVersion,
    const nsTArray<RefPtr<GfxDriverInfo>>& aDriverInfo,
    nsACString& aFailureId, OperatingSystem* aOS) {
  NS_ENSURE_ARG_POINTER(aStatus);
  aSuggestedDriverVersion.SetIsVoid(true);
  *aStatus = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;
  OperatingSystem os = OperatingSystem::Linux;
  if (aOS) {
    *aOS = os;
  }

  // DMABuf: enable when we have a usable DRM render node (GBM path).
  if (aFeature == nsIGfxInfo::FEATURE_DMABUF ||
      aFeature == nsIGfxInfo::FEATURE_DMABUF_WEBGL) {
    DiscoverDrmRenderDevice();
    if (mDrmRenderDevice.IsEmpty()) {
      *aStatus = nsIGfxInfo::FEATURE_BLOCKED_DEVICE;
      aFailureId = "FEATURE_FAILURE_NO_DRM_DEVICE"_ns;
    } else {
      *aStatus = nsIGfxInfo::FEATURE_STATUS_OK;
    }
    return NS_OK;
  }

  // Per-codec VAAPI status.
  static const struct {
    int32_t mFeature;
    int mCodec;
    const char* mFailureId;
  } kFeatureToCodecs[] = {
      {nsIGfxInfo::FEATURE_H264_HW_DECODE, CODEC_HW_DEC_H264,
       "FEATURE_FAILURE_VIDEO_DECODING_MISSING"},
      {nsIGfxInfo::FEATURE_H264_HW_ENCODE, CODEC_HW_ENC_H264,
       "FEATURE_FAILURE_VIDEO_ENCODING_MISSING"},
      {nsIGfxInfo::FEATURE_VP8_HW_DECODE, CODEC_HW_DEC_VP8,
       "FEATURE_FAILURE_VIDEO_DECODING_MISSING"},
      {nsIGfxInfo::FEATURE_VP8_HW_ENCODE, CODEC_HW_ENC_VP8,
       "FEATURE_FAILURE_VIDEO_ENCODING_MISSING"},
      {nsIGfxInfo::FEATURE_VP9_HW_DECODE, CODEC_HW_DEC_VP9,
       "FEATURE_FAILURE_VIDEO_DECODING_MISSING"},
      {nsIGfxInfo::FEATURE_VP9_HW_ENCODE, CODEC_HW_ENC_VP9,
       "FEATURE_FAILURE_VIDEO_ENCODING_MISSING"},
      {nsIGfxInfo::FEATURE_AV1_HW_DECODE, CODEC_HW_DEC_AV1,
       "FEATURE_FAILURE_VIDEO_DECODING_MISSING"},
      {nsIGfxInfo::FEATURE_AV1_HW_ENCODE, CODEC_HW_ENC_AV1,
       "FEATURE_FAILURE_VIDEO_ENCODING_MISSING"},
      {nsIGfxInfo::FEATURE_HEVC_HW_DECODE, CODEC_HW_DEC_HEVC,
       "FEATURE_FAILURE_VIDEO_DECODING_MISSING"},
      {nsIGfxInfo::FEATURE_HEVC_HW_ENCODE, CODEC_HW_ENC_HEVC,
       "FEATURE_FAILURE_VIDEO_ENCODING_MISSING"},
  };

  for (const auto& pair : kFeatureToCodecs) {
    if (aFeature != pair.mFeature) {
      continue;
    }
    GetDataVAAPI();
    if (mVAAPISupportedCodecs & pair.mCodec) {
      *aStatus = nsIGfxInfo::FEATURE_STATUS_OK;
    } else {
      *aStatus = nsIGfxInfo::FEATURE_BLOCKED_PLATFORM_TEST;
      aFailureId = pair.mFailureId;
    }
    return NS_OK;
  }

  // Handle HW video before GfxInfoBase: empty vendor IDs would otherwise
  // block us via OnlyAllowFeatureOnKnownConfig.
  if (aFeature == nsIGfxInfo::FEATURE_HARDWARE_VIDEO_DECODING ||
      aFeature == nsIGfxInfo::FEATURE_HARDWARE_VIDEO_ENCODING) {
    if (!StaticPrefs::media_hardware_video_decoding_enabled_AtStartup() &&
        aFeature == nsIGfxInfo::FEATURE_HARDWARE_VIDEO_DECODING) {
      *aStatus = nsIGfxInfo::FEATURE_BLOCKED_DEVICE;
      aFailureId = "FEATURE_FAILURE_VIDEO_DECODING_DISABLED"_ns;
      return NS_OK;
    }
    GetDataVAAPI();
    if (mIsVAAPISupported.valueOr(false)) {
      *aStatus = nsIGfxInfo::FEATURE_STATUS_OK;
    } else {
      *aStatus = nsIGfxInfo::FEATURE_BLOCKED_PLATFORM_TEST;
      aFailureId = "FEATURE_FAILURE_VIDEO_DECODING_TEST_FAILED"_ns;
    }
    return NS_OK;
  }

  return GfxInfoBase::GetFeatureStatusImpl(
      aFeature, aStatus, aSuggestedDriverVersion, aDriverInfo, aFailureId, aOS);
}

#ifdef DEBUG
NS_IMETHODIMP GfxInfo::SpoofVendorID(const nsAString&) { return NS_OK; }
NS_IMETHODIMP GfxInfo::SpoofDeviceID(const nsAString&) { return NS_OK; }
NS_IMETHODIMP GfxInfo::SpoofDriverVersion(const nsAString&) { return NS_OK; }
NS_IMETHODIMP GfxInfo::SpoofOSVersion(uint32_t) { return NS_OK; }
NS_IMETHODIMP GfxInfo::SpoofOSVersionEx(uint32_t, uint32_t, uint32_t,
                                        uint32_t) {
  return NS_OK;
}
#endif


const nsTArray<uint64_t>& GfxInfo::GetDMABufEGLModifiers(
    uint32_t /*aDrmFourcc*/) const {
  static const nsTArray<uint64_t> empty;
  return empty;
}

}  // namespace mozilla::widget
