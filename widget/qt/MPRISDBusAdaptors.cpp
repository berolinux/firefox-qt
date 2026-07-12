/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MPRISDBusAdaptors.h"
#include "MPRISServiceHandler.h"

#include "mozilla/dom/MediaControlKeySource.h"

using mozilla::dom::MediaControlAction;
using mozilla::dom::MediaControlActionParams;
using mozilla::dom::MediaControlKey;

namespace mozilla::widget {

MPRISRootAdaptor::MPRISRootAdaptor(QObject* aParent,
                                   MPRISServiceHandler* aHandler)
    : QDBusAbstractAdaptor(aParent), mHandler(aHandler) {
  setAutoRelaySignals(true);
}

bool MPRISRootAdaptor::canRaise() const {
  return mHandler && mHandler->IsMediaKeySupported(MediaControlKey::Focus);
}

QString MPRISRootAdaptor::identity() const {
  return mHandler ? mHandler->Identity() : QString();
}

QString MPRISRootAdaptor::desktopEntry() const {
  return mHandler ? mHandler->DesktopEntry() : QString();
}

void MPRISRootAdaptor::Raise() {
  if (mHandler) {
    mHandler->PressKey(MediaControlAction(MediaControlKey::Focus));
  }
}

void MPRISRootAdaptor::Quit() {}

MPRISPlayerAdaptor::MPRISPlayerAdaptor(QObject* aParent,
                                       MPRISServiceHandler* aHandler)
    : QDBusAbstractAdaptor(aParent), mHandler(aHandler) {
  setAutoRelaySignals(true);
}

QString MPRISPlayerAdaptor::playbackStatus() const {
  return mHandler ? mHandler->PlaybackStatusString() : QStringLiteral("Stopped");
}

double MPRISPlayerAdaptor::rate() const {
  return mHandler ? mHandler->PlaybackRate() : 1.0;
}

QVariantMap MPRISPlayerAdaptor::metadata() const {
  return mHandler ? mHandler->MetadataMap() : QVariantMap();
}

double MPRISPlayerAdaptor::volume() const {
  return mHandler ? mHandler->Volume() : 1.0;
}

void MPRISPlayerAdaptor::setVolume(double aVolume) {
  if (mHandler) {
    mHandler->SetVolumeFromDBus(aVolume);
  }
}

qlonglong MPRISPlayerAdaptor::position() const {
  return mHandler ? mHandler->PositionMicroseconds() : 0;
}

bool MPRISPlayerAdaptor::canGoNext() const {
  return mHandler && mHandler->IsMediaKeySupported(MediaControlKey::Nexttrack);
}

bool MPRISPlayerAdaptor::canGoPrevious() const {
  return mHandler &&
         mHandler->IsMediaKeySupported(MediaControlKey::Previoustrack);
}

bool MPRISPlayerAdaptor::canPlay() const {
  return mHandler && mHandler->IsMediaKeySupported(MediaControlKey::Play);
}

bool MPRISPlayerAdaptor::canPause() const {
  return mHandler && mHandler->IsMediaKeySupported(MediaControlKey::Pause);
}

bool MPRISPlayerAdaptor::canSeek() const {
  return mHandler && mHandler->IsMediaKeySupported(MediaControlKey::Seekto);
}

bool MPRISPlayerAdaptor::Press(MediaControlKey aKey) {
  return mHandler && mHandler->PressKey(MediaControlAction(aKey));
}

void MPRISPlayerAdaptor::Next() { Press(MediaControlKey::Nexttrack); }
void MPRISPlayerAdaptor::Previous() { Press(MediaControlKey::Previoustrack); }
void MPRISPlayerAdaptor::Pause() { Press(MediaControlKey::Pause); }
void MPRISPlayerAdaptor::PlayPause() { Press(MediaControlKey::Playpause); }
void MPRISPlayerAdaptor::Stop() { Press(MediaControlKey::Stop); }
void MPRISPlayerAdaptor::Play() { Press(MediaControlKey::Play); }

void MPRISPlayerAdaptor::Seek(qlonglong aOffset) {
  if (!mHandler) {
    return;
  }
  const double seconds = static_cast<double>(aOffset) / 1.0e6;
  if (seconds >= 0.0) {
    mHandler->PressKey(MediaControlAction(
        MediaControlKey::Seekforward, MediaControlActionParams(seconds)));
  } else {
    mHandler->PressKey(MediaControlAction(
        MediaControlKey::Seekbackward, MediaControlActionParams(-seconds)));
  }
}

void MPRISPlayerAdaptor::SetPosition(const QDBusObjectPath&,
                                     qlonglong aPosition) {
  if (!mHandler) {
    return;
  }
  const double seconds = static_cast<double>(aPosition) / 1.0e6;
  mHandler->PressKey(MediaControlAction(
      MediaControlKey::Seekto, MediaControlActionParams(seconds, false)));
}

}  // namespace mozilla::widget
