/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MPRISServiceHandler.h"
#include "MPRISDBusAdaptors.h"

#include "imgIContainer.h"
#include "mozilla/dom/MediaControlUtils.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/image/FetchDecodedImage.h"
#include "mozilla/Logging.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"
#include "mozilla/XREAppData.h"
#include "nsContentUtils.h"
#include "nsDirectoryServiceDefs.h"
#include "nsIOutputStream.h"
#include "nsIXULAppInfo.h"
#include "nsNetUtil.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"
#include "prenv.h"
#include "prio.h"

#include <unistd.h>

#pragma GCC visibility push(default)
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QObject>
#include <QVariant>
#pragma GCC visibility pop

// NOTE: gAppData will be null in embedded contexts.
extern const mozilla::XREAppData* gAppData;

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::widget;

static LazyLogModule gQtMPRISLog("QtMPRIS");
#define LOGMPRIS(...) \
  MOZ_LOG(gQtMPRISLog, LogLevel::Debug, ("MPRIS %p: " __VA_ARGS__, this))

static constexpr char kObjectPath[] = "/org/mpris/MediaPlayer2";
static constexpr char kPlayerIface[] = "org.mpris.MediaPlayer2.Player";
static constexpr char kRootIface[] = "org.mpris.MediaPlayer2";
static constexpr char kTrackPath[] = "/org/mpris/MediaPlayer2/firefox";

// Unique filenames avoid desktop-shell caching of stale album art (see GTK).
static uint32_t gImageNumber = 0;

// Firefox 160 removed dom::IsValidImageUrl. MPRIS still publishes only
// remote http(s) artwork URLs; local art is a file:// path we write ourselves.
static bool IsValidImageUrl(const nsAString& aUrl) {
  return StringBeginsWith(aUrl, u"http://"_ns) ||
         StringBeginsWith(aUrl, u"https://"_ns);
}

static bool QtIsRunningUnderFlatpak() {
  // Match GTK: presence of /.flatpak-info in the sandbox root.
  return access("/.flatpak-info", F_OK) == 0;
}

MPRISServiceHandler::MPRISServiceHandler() = default;

MPRISServiceHandler::~MPRISServiceHandler() {
  MOZ_ASSERT(!mInitialized, "Close() must be called before destroy");
}

void MPRISServiceHandler::InitIdentity() {
  mIdentity = QStringLiteral("Mozilla");
  mDesktopEntry = QStringLiteral("firefox");

  nsCOMPtr<nsIXULAppInfo> appInfo =
      do_GetService("@mozilla.org/xre/app-info;1");
  if (appInfo) {
    nsAutoCString vendor;
    nsAutoCString name;
    if (NS_SUCCEEDED(appInfo->GetVendor(vendor)) && !vendor.IsEmpty()) {
      mIdentity = QString::fromUtf8(vendor.get(), vendor.Length());
    }
    if (gAppData && gAppData->remotingName) {
      mDesktopEntry = QString::fromUtf8(gAppData->remotingName, -1);
    } else if (NS_SUCCEEDED(appInfo->GetName(name)) && !name.IsEmpty()) {
      mDesktopEntry = QString::fromUtf8(name.get(), name.Length());
    }
  }
  mIdentity += QLatin1Char(' ');
  mIdentity += mDesktopEntry;
}

bool MPRISServiceHandler::RegisterOnSessionBus() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    LOGMPRIS("session bus not connected");
    return false;
  }

  InitIdentity();

  // Unique service name per process (matches GTK idea of multi-instance).
  mServiceName =
      QStringLiteral("org.mpris.MediaPlayer2.firefox.instance_%1").arg(getpid());

  mDBusRoot = new QObject();
  mRootAdaptor = new MPRISRootAdaptor(mDBusRoot, this);
  mPlayerAdaptor = new MPRISPlayerAdaptor(mDBusRoot, this);

  if (!bus.registerObject(QString::fromLatin1(kObjectPath), mDBusRoot)) {
    LOGMPRIS("registerObject failed: %s",
             qPrintable(bus.lastError().message()));
    delete mDBusRoot;
    mDBusRoot = nullptr;
    mRootAdaptor = nullptr;
    mPlayerAdaptor = nullptr;
    return false;
  }

  if (!bus.registerService(mServiceName)) {
    LOGMPRIS("registerService(%s) failed: %s", qPrintable(mServiceName),
             qPrintable(bus.lastError().message()));
    bus.unregisterObject(QString::fromLatin1(kObjectPath));
    delete mDBusRoot;
    mDBusRoot = nullptr;
    mRootAdaptor = nullptr;
    mPlayerAdaptor = nullptr;
    return false;
  }

  LOGMPRIS("registered %s", qPrintable(mServiceName));
  return true;
}

void MPRISServiceHandler::UnregisterFromSessionBus() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!mServiceName.isEmpty()) {
    bus.unregisterService(mServiceName);
    mServiceName.clear();
  }
  bus.unregisterObject(QString::fromLatin1(kObjectPath));
  delete mDBusRoot;
  mDBusRoot = nullptr;
  mRootAdaptor = nullptr;
  mPlayerAdaptor = nullptr;
}

bool MPRISServiceHandler::Open() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mInitialized);
  if (!RegisterOnSessionBus()) {
    return false;
  }
  mInitialized = true;
  return true;
}

void MPRISServiceHandler::Close() {
  if (!mInitialized) {
    return;
  }
  SetPlaybackState(MediaSessionPlaybackState::None);
  mTitle.Truncate();
  mArtist.Truncate();
  mAlbum.Truncate();
  mUrl.Truncate();
  mDurationSeconds = 0.0;
  mSupportedKeys = 0;
  mPositionState.reset();
  ClearArtwork();
  UnregisterFromSessionBus();
  mInitialized = false;
  MediaControlKeySource::Close();
}

void MPRISServiceHandler::EmitEvent(const MediaControlAction& aAction) const {
  for (const auto& listener : mListeners) {
    listener->OnActionPerformed(aAction);
  }
}

bool MPRISServiceHandler::PressKey(const MediaControlAction& aAction) {
  if (!mInitialized || aAction.mKey.isNothing()) {
    return false;
  }
  if (!IsMediaKeySupported(aAction.mKey.value())) {
    LOGMPRIS("key not supported");
    return false;
  }
  EmitEvent(aAction);
  return true;
}

bool MPRISServiceHandler::IsMediaKeySupported(MediaControlKey aKey) const {
  return mSupportedKeys & KeyMask(aKey);
}

void MPRISServiceHandler::SetVolumeFromDBus(double aVolume) {
  if (mVolume == aVolume) {
    return;
  }
  mVolume = aVolume;
  EmitEvent(MediaControlAction(MediaControlKey::Setvolume,
                                MediaControlActionParams::FromVolume(mVolume)));
  if (mVolume > 0.0) {
    EmitEvent(MediaControlAction(MediaControlKey::Unmute));
  }
  EmitPropertiesChanged(QString::fromLatin1(kPlayerIface),
                        {{QStringLiteral("Volume"), QVariant(mVolume)}});
}

QString MPRISServiceHandler::PlaybackStatusString() const {
  switch (GetPlaybackState()) {
    case MediaSessionPlaybackState::Playing:
      return QStringLiteral("Playing");
    case MediaSessionPlaybackState::Paused:
      return QStringLiteral("Paused");
    case MediaSessionPlaybackState::None:
    default:
      return QStringLiteral("Stopped");
  }
}

QVariantMap MPRISServiceHandler::MetadataMap() const {
  QVariantMap map;
  map.insert(QStringLiteral("mpris:trackid"),
             QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kTrackPath))));
  map.insert(QStringLiteral("xesam:title"),
             QString::fromUtf16(mTitle.get(), mTitle.Length()));
  map.insert(QStringLiteral("xesam:album"),
             QString::fromUtf16(mAlbum.get(), mAlbum.Length()));
  map.insert(QStringLiteral("xesam:artist"),
             QStringList{QString::fromUtf16(mArtist.get(), mArtist.Length())});
  if (!mUrl.IsEmpty()) {
    map.insert(QStringLiteral("xesam:url"),
               QString::fromUtf8(mUrl.get(), mUrl.Length()));
  }
  if (mDurationSeconds > 0.0) {
    const qlonglong us =
        static_cast<qlonglong>(mDurationSeconds * 1.0e6);
    map.insert(QStringLiteral("mpris:length"), QVariant::fromValue(us));
  }
  if (!mArtUrl.IsEmpty()) {
    map.insert(QStringLiteral("mpris:artUrl"),
               QString::fromUtf8(mArtUrl.get(), mArtUrl.Length()));
  }
  return map;
}

qlonglong MPRISServiceHandler::PositionMicroseconds() const {
  if (mPositionState.isNothing()) {
    return 0;
  }
  return static_cast<qlonglong>(mPositionState->CurrentPlaybackPosition() *
                                1.0e6);
}

void MPRISServiceHandler::EmitPropertiesChanged(
    const QString& aInterface, const QVariantMap& aChanged) const {
  if (!mInitialized) {
    return;
  }
  QDBusMessage signal = QDBusMessage::createSignal(
      QString::fromLatin1(kObjectPath),
      QStringLiteral("org.freedesktop.DBus.Properties"),
      QStringLiteral("PropertiesChanged"));
  signal << aInterface << aChanged << QStringList();
  QDBusConnection::sessionBus().send(signal);
}

void MPRISServiceHandler::EmitSeeked() const {
  if (!mInitialized || !mPlayerAdaptor) {
    return;
  }
  Q_EMIT mPlayerAdaptor->Seeked(PositionMicroseconds());
}

void MPRISServiceHandler::SetPlaybackState(MediaSessionPlaybackState aState) {
  if (mPlaybackState == aState) {
    return;
  }
  MediaControlKeySource::SetPlaybackState(aState);
  EmitPropertiesChanged(
      QString::fromLatin1(kPlayerIface),
      {{QStringLiteral("PlaybackStatus"), PlaybackStatusString()}});
}

void MPRISServiceHandler::EmitMetadataChanged() const {
  EmitPropertiesChanged(QString::fromLatin1(kPlayerIface),
                        {{QStringLiteral("Metadata"), MetadataMap()}});
}

void MPRISServiceHandler::ClearArtwork() {
  mArtFetchGeneration++;
  RemoveAllLocalImages();
  mArtUrl.Truncate();
  mCurrentImageUrl.Truncate();
}

bool MPRISServiceHandler::TryExportSurfaceArt(const MediaImageData& aImage) {
  if (!aImage.mDataSurface) {
    return false;
  }

  // Same source as last successful export: keep existing file:// artUrl.
  if (mCurrentImageUrl == aImage.mSrc && !mArtUrl.IsEmpty() &&
      StringBeginsWith(mArtUrl, "file://"_ns)) {
    LOGMPRIS("keeping existing local art for same source");
    return true;
  }

  uint32_t size = 0;
  char* data = nullptr;
  nsCOMPtr<nsIInputStream> inputStream;
  nsresult rv =
      GetEncodedImageBuffer(aImage.mDataSurface, mMimeType,
                            getter_AddRefs(inputStream), &size, &data);
  if (NS_FAILED(rv) || !inputStream || size == 0 || !data) {
    LOGMPRIS("failed to encode artwork surface");
    return false;
  }

  if (!SetImageToDisplay(data, size)) {
    return false;
  }
  mCurrentImageUrl = aImage.mSrc;
  LOGMPRIS("local art from surface src=%s",
           NS_ConvertUTF16toUTF8(mCurrentImageUrl).get());
  return true;
}

void MPRISServiceHandler::SetRemoteArtUrl(const nsAString& aSrc) {
  CopyUTF16toUTF8(aSrc, mArtUrl);
  mCurrentImageUrl.Assign(aSrc);
  LOGMPRIS("remote artUrl %s", mArtUrl.get());
  EmitMetadataChanged();
}

void MPRISServiceHandler::FetchRemoteArtwork(const nsAString& aSrc) {
  nsCOMPtr<nsIURI> uri;
  if (NS_FAILED(NS_NewURI(getter_AddRefs(uri), aSrc))) {
    return;
  }

  const uint32_t gen = ++mArtFetchGeneration;
  LOGMPRIS("parent-side fetch art %s gen=%u",
           NS_ConvertUTF16toUTF8(aSrc).get(), gen);

  // Parent process load: SEC_ALLOW_CROSS_ORIGIN so CDN thumbs work without
  // the content principal (YouTube often never delivers mDataSurface here).
  RefPtr<image::FetchDecodedImagePromise> promise = image::FetchDecodedImage(
      uri, gfx::IntSize{}, nsContentUtils::GetSystemPrincipal());
  promise->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [self = RefPtr{this}, gen,
       src = nsString(aSrc)](already_AddRefed<imgIContainer> aImage) {
        if (gen != self->mArtFetchGeneration) {
          return;
        }
        nsCOMPtr<imgIContainer> image(std::move(aImage));
        RefPtr<gfx::SourceSurface> surface = image->GetFrame(
            imgIContainer::FRAME_CURRENT,
            imgIContainer::FLAG_SYNC_DECODE | imgIContainer::FLAG_ASYNC_NOTIFY);
        if (!surface) {
          return;
        }
        RefPtr<gfx::DataSourceSurface> dataSurface = surface->GetDataSurface();
        if (!dataSurface) {
          return;
        }
        MediaImageData fake;
        fake.mSrc = src;
        fake.mDataSurface = std::move(dataSurface);
        if (self->TryExportSurfaceArt(fake)) {
          self->EmitMetadataChanged();
        }
      },
      [self = RefPtr{this}, gen](const nsresult&) {
        (void)self;
        (void)gen;
      });
}

void MPRISServiceHandler::SetMediaMetadata(
    const MediaMetadataBase& aMetadata) {
  mTitle = aMetadata.mTitle;
  mArtist = aMetadata.mArtist;
  mAlbum = aMetadata.mAlbum;
  mUrl = aMetadata.mUrl;

  LOGMPRIS("SetMediaMetadata artwork count=%zu", aMetadata.mArtwork.Length());

  bool haveArt = false;
  for (const MediaImageData& image : aMetadata.mArtwork) {
    if (TryExportSurfaceArt(image)) {
      haveArt = true;
      break;
    }
  }

  if (!haveArt) {
    // Content often sends only remote src (no mDataSurface) — publish the URL
    // immediately so shells that load http(s) artUrl show a thumb, then try
    // parent-side decode for a local file:// artUrl.
    for (const MediaImageData& image : aMetadata.mArtwork) {
      if (!IsValidImageUrl(image.mSrc)) {
        continue;
      }
      // Skip re-fetch if we already point at this remote URL.
      if (mCurrentImageUrl != image.mSrc || mArtUrl.IsEmpty()) {
        SetRemoteArtUrl(image.mSrc);
      }
      FetchRemoteArtwork(image.mSrc);
      haveArt = true;
      break;
    }
  }

  if (!haveArt) {
    // No usable artwork for this metadata: clear stale art from previous track.
    if (!mArtUrl.IsEmpty() || !mCurrentImageUrl.IsEmpty()) {
      ClearArtwork();
    }
  }

  EmitMetadataChanged();
}

bool MPRISServiceHandler::SetImageToDisplay(const char* aImageData,
                                            uint32_t aDataSize) {
  if (!RenewLocalImageFile(aImageData, aDataSize)) {
    return false;
  }
  MOZ_ASSERT(mLocalImageFile);
  mArtUrl = "file://"_ns;
  mArtUrl.Append(mLocalImageFile->NativePath());
  LOGMPRIS("art file %s", mArtUrl.get());
  EmitMetadataChanged();
  return true;
}

bool MPRISServiceHandler::RenewLocalImageFile(const char* aImageData,
                                              uint32_t aDataSize) {
  MOZ_ASSERT(aImageData);
  MOZ_ASSERT(aDataSize != 0);
  if (!InitLocalImageFile()) {
    return false;
  }
  MOZ_ASSERT(mLocalImageFile);
  nsCOMPtr<nsIOutputStream> out;
  nsresult rv =
      NS_NewLocalFileOutputStream(getter_AddRefs(out), mLocalImageFile,
                                  PR_RDWR | PR_CREATE_FILE | PR_TRUNCATE);
  uint32_t written = 0;
  if (NS_SUCCEEDED(rv)) {
    rv = out->Write(aImageData, aDataSize, &written);
  }
  if (NS_FAILED(rv) || written != aDataSize) {
    LOGMPRIS("failed to write art file");
    RemoveAllLocalImages();
    return false;
  }
  return true;
}

bool MPRISServiceHandler::InitLocalImageFile() {
  RemoveAllLocalImages();
  if (!InitLocalImageFolder()) {
    return false;
  }
  MOZ_ASSERT(mLocalImageFolder);
  MOZ_ASSERT(!mLocalImageFile);
  nsresult rv = mLocalImageFolder->Clone(getter_AddRefs(mLocalImageFile));
  if (NS_FAILED(rv)) {
    return false;
  }

  auto cleanup = MakeScopeExit([this, self = RefPtr{this}] {
    mLocalImageFile = nullptr;
  });

  char filename[64];
  SprintfLiteral(filename, "%d_%u.png", getpid(), gImageNumber++);
  rv = mLocalImageFile->Append(NS_ConvertUTF8toUTF16(filename));
  if (NS_FAILED(rv)) {
    return false;
  }
  rv = mLocalImageFile->Create(nsIFile::NORMAL_FILE_TYPE, 0600);
  if (NS_FAILED(rv)) {
    return false;
  }
  cleanup.release();
  return true;
}

bool MPRISServiceHandler::InitLocalImageFolder() {
  if (mLocalImageFolder && LocalImageFolderExists()) {
    return true;
  }

  nsresult rv = NS_ERROR_FAILURE;
  if (QtIsRunningUnderFlatpak()) {
    // XDG_DATA_HOME is the same path in host and sandbox (GTK MPRIS behavior).
    if (const char* xdgDataHome = PR_GetEnv("XDG_DATA_HOME")) {
      rv = NS_NewNativeLocalFile(nsDependentCString(xdgDataHome),
                                 getter_AddRefs(mLocalImageFolder));
    }
  } else {
    rv = NS_GetSpecialDirectory(XRE_USER_APP_DATA_DIR,
                                getter_AddRefs(mLocalImageFolder));
  }

  if (NS_FAILED(rv) || !mLocalImageFolder) {
    LOGMPRIS("failed to resolve art folder");
    mLocalImageFolder = nullptr;
    return false;
  }

  auto cleanup = MakeScopeExit([&] { mLocalImageFolder = nullptr; });

  rv = mLocalImageFolder->Append(u"firefox-mpris"_ns);
  if (NS_FAILED(rv)) {
    return false;
  }
  if (!LocalImageFolderExists()) {
    rv = mLocalImageFolder->Create(nsIFile::DIRECTORY_TYPE, 0700);
    if (NS_FAILED(rv)) {
      LOGMPRIS("failed to create art folder");
      return false;
    }
  }
  cleanup.release();
  return true;
}

void MPRISServiceHandler::RemoveAllLocalImages() {
  if (!mLocalImageFolder || !LocalImageFolderExists()) {
    mLocalImageFile = nullptr;
    mLocalImageFolder = nullptr;
    mArtUrl.Truncate();
    return;
  }
  (void)mLocalImageFolder->Remove(/* aRecursive */ true);
  mLocalImageFile = nullptr;
  mLocalImageFolder = nullptr;
  mArtUrl.Truncate();
}

bool MPRISServiceHandler::LocalImageFolderExists() const {
  if (!mLocalImageFolder) {
    return false;
  }
  bool exists = false;
  return NS_SUCCEEDED(mLocalImageFolder->Exists(&exists)) && exists;
}

void MPRISServiceHandler::SetSupportedMediaKeys(
    const MediaKeysArray& aSupportedKeys) {
  uint32_t supported = 0;
  for (const MediaControlKey& key : aSupportedKeys) {
    supported |= KeyMask(key);
  }
  if (mSupportedKeys == supported) {
    return;
  }
  const uint32_t old = mSupportedKeys;
  mSupportedKeys = supported;

  auto emitIf = [&](MediaControlKey key, const char* prop) {
    const bool was = old & KeyMask(key);
    const bool now = mSupportedKeys & KeyMask(key);
    if (was != now) {
      const char* iface = (key == MediaControlKey::Focus) ? kRootIface
                                                          : kPlayerIface;
      EmitPropertiesChanged(QString::fromLatin1(iface),
                            {{QString::fromLatin1(prop), now}});
    }
  };
  emitIf(MediaControlKey::Focus, "CanRaise");
  emitIf(MediaControlKey::Nexttrack, "CanGoNext");
  emitIf(MediaControlKey::Previoustrack, "CanGoPrevious");
  emitIf(MediaControlKey::Play, "CanPlay");
  emitIf(MediaControlKey::Pause, "CanPause");
  emitIf(MediaControlKey::Seekto, "CanSeek");
}

void MPRISServiceHandler::SetPositionState(
    const Maybe<PositionState>& aState) {
  bool rateChanged = false;
  bool durationChanged = false;
  const bool positionChanged =
      mPositionState.isSome() || aState.isSome();
  if (mPositionState.isSome() && aState.isSome()) {
    rateChanged = mPositionState->mPlaybackRate != aState->mPlaybackRate;
    durationChanged = mPositionState->mDuration != aState->mDuration;
  } else if (mPositionState.isNothing() && aState.isSome()) {
    rateChanged = aState->mPlaybackRate != 1.0;
    durationChanged = true;
  } else if (mPositionState.isSome() && aState.isNothing()) {
    rateChanged = mPositionState->mPlaybackRate != 1.0;
    durationChanged = true;
  }

  mPositionState = aState;
  mPlaybackRate = aState ? aState->mPlaybackRate : 1.0;
  mDurationSeconds = aState ? aState->mDuration : 0.0;

  QVariantMap changed;
  if (rateChanged) {
    changed.insert(QStringLiteral("Rate"), mPlaybackRate);
  }
  if (durationChanged) {
    changed.insert(QStringLiteral("Metadata"), MetadataMap());
  }
  if (!changed.isEmpty()) {
    EmitPropertiesChanged(QString::fromLatin1(kPlayerIface), changed);
  }
  if (positionChanged) {
    EmitSeeked();
  }
}
