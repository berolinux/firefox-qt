/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_MPRISDBusAdaptors_h
#define widget_qt_MPRISDBusAdaptors_h

#include "mozilla/dom/MediaControlKeySource.h"

#pragma GCC visibility push(default)
#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#pragma GCC visibility pop

namespace mozilla::widget {

class MPRISServiceHandler;

class MPRISRootAdaptor final : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
  Q_PROPERTY(bool CanQuit READ canQuit)
  Q_PROPERTY(bool CanRaise READ canRaise)
  Q_PROPERTY(bool HasTrackList READ hasTrackList)
  Q_PROPERTY(QString Identity READ identity)
  Q_PROPERTY(QString DesktopEntry READ desktopEntry)
  Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
  Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

 public:
  MPRISRootAdaptor(QObject* aParent, MPRISServiceHandler* aHandler);

  bool canQuit() const { return false; }
  bool canRaise() const;
  bool hasTrackList() const { return false; }
  QString identity() const;
  QString desktopEntry() const;
  QStringList supportedUriSchemes() const { return {}; }
  QStringList supportedMimeTypes() const { return {}; }

 public Q_SLOTS:
  void Raise();
  void Quit();

 private:
  MPRISServiceHandler* mHandler;
};

class MPRISPlayerAdaptor final : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
  Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
  Q_PROPERTY(double Rate READ rate WRITE setRate)
  Q_PROPERTY(QVariantMap Metadata READ metadata)
  Q_PROPERTY(double Volume READ volume WRITE setVolume)
  Q_PROPERTY(qlonglong Position READ position)
  Q_PROPERTY(double MinimumRate READ minimumRate)
  Q_PROPERTY(double MaximumRate READ maximumRate)
  Q_PROPERTY(bool CanGoNext READ canGoNext)
  Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
  Q_PROPERTY(bool CanPlay READ canPlay)
  Q_PROPERTY(bool CanPause READ canPause)
  Q_PROPERTY(bool CanSeek READ canSeek)
  Q_PROPERTY(bool CanControl READ canControl)

 public:
  MPRISPlayerAdaptor(QObject* aParent, MPRISServiceHandler* aHandler);

  QString playbackStatus() const;
  double rate() const;
  void setRate(double) {}
  QVariantMap metadata() const;
  double volume() const;
  void setVolume(double aVolume);
  qlonglong position() const;
  double minimumRate() const { return 1.0; }
  double maximumRate() const { return 1.0; }
  bool canGoNext() const;
  bool canGoPrevious() const;
  bool canPlay() const;
  bool canPause() const;
  bool canSeek() const;
  bool canControl() const { return true; }

 public Q_SLOTS:
  void Next();
  void Previous();
  void Pause();
  void PlayPause();
  void Stop();
  void Play();
  void Seek(qlonglong aOffset);
  void SetPosition(const QDBusObjectPath& aTrackId, qlonglong aPosition);
  void OpenUri(const QString&) {}

 Q_SIGNALS:
  void Seeked(qlonglong Position);

 private:
  bool Press(mozilla::dom::MediaControlKey aKey);
  MPRISServiceHandler* mHandler;
};

}  // namespace mozilla::widget

#endif
