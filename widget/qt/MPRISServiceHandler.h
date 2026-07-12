/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_MPRISServiceHandler_h
#define widget_qt_MPRISServiceHandler_h

#include "mozilla/dom/MediaControlKeySource.h"
#include "mozilla/Maybe.h"
#include "nsIFile.h"
#include "nsMimeTypes.h"
#include "nsString.h"

class QObject;

namespace mozilla::widget {

class MPRISRootAdaptor;
class MPRISPlayerAdaptor;

/**
 * MPRIS (org.mpris.MediaPlayer2) over QtDBus so desktop shells can control
 * Firefox media playback (play/pause/next/…) and show metadata.
 */
class MPRISServiceHandler final : public dom::MediaControlKeySource {
  NS_INLINE_DECL_REFCOUNTING(MPRISServiceHandler, override)

 public:
  MPRISServiceHandler();

  bool Open() override;
  void Close() override;
  bool IsOpened() const override { return mInitialized; }

  void SetPlaybackState(dom::MediaSessionPlaybackState aState) override;
  void SetMediaMetadata(const dom::MediaMetadataBase& aMetadata) override;
  void SetSupportedMediaKeys(const MediaKeysArray& aSupportedKeys) override;
  void SetPositionState(const Maybe<dom::PositionState>& aState) override;

  // Called from D-Bus adaptors (main thread).
  bool PressKey(const dom::MediaControlAction& aAction);
  bool IsMediaKeySupported(dom::MediaControlKey aKey) const;
  void SetVolumeFromDBus(double aVolume);

  QString PlaybackStatusString() const;
  QVariantMap MetadataMap() const;
  qlonglong PositionMicroseconds() const;
  double PlaybackRate() const { return mPlaybackRate; }
  double Volume() const { return mVolume; }

  QString Identity() const { return mIdentity; }
  QString DesktopEntry() const { return mDesktopEntry; }

  void EmitPropertiesChanged(const QString& aInterface,
                             const QVariantMap& aChanged) const;
  void EmitSeeked() const;

 private:
  ~MPRISServiceHandler() override;

  void InitIdentity();
  void EmitEvent(const dom::MediaControlAction& aAction) const;
  bool RegisterOnSessionBus();
  void UnregisterFromSessionBus();

  void EmitMetadataChanged() const;
  void ClearArtwork();
  bool SetImageToDisplay(const char* aImageData, uint32_t aDataSize);
  bool RenewLocalImageFile(const char* aImageData, uint32_t aDataSize);
  bool InitLocalImageFile();
  bool InitLocalImageFolder();
  void RemoveAllLocalImages();
  bool LocalImageFolderExists() const;
  // Prefer decoded surfaces; fall back to remote URL / parent-side fetch.
  bool TryExportSurfaceArt(const dom::MediaImageData& aImage);
  void SetRemoteArtUrl(const nsAString& aSrc);
  void FetchRemoteArtwork(const nsAString& aSrc);

  static constexpr uint32_t KeyMask(dom::MediaControlKey aKey) {
    return 1u << static_cast<uint32_t>(aKey);
  }

  bool mInitialized = false;
  uint32_t mSupportedKeys = 0;
  double mVolume = 1.0;
  double mPlaybackRate = 1.0;
  Maybe<dom::PositionState> mPositionState;

  nsString mTitle;
  nsString mArtist;
  nsString mAlbum;
  nsCString mUrl;
  double mDurationSeconds = 0.0;

  // Album art written under a per-user cache dir as PNG (same as GTK MPRIS).
  nsCString mMimeType{IMAGE_PNG};
  nsCString mArtUrl;
  nsString mCurrentImageUrl;
  nsCOMPtr<nsIFile> mLocalImageFile;
  nsCOMPtr<nsIFile> mLocalImageFolder;
  // Bumped to drop stale async FetchDecodedImage results.
  uint32_t mArtFetchGeneration = 0;

  QString mIdentity;
  QString mDesktopEntry;
  QString mServiceName;

  // QObject tree root; owns adaptors.
  QObject* mDBusRoot = nullptr;
  MPRISRootAdaptor* mRootAdaptor = nullptr;
  MPRISPlayerAdaptor* mPlayerAdaptor = nullptr;
};

}  // namespace mozilla::widget

#endif
