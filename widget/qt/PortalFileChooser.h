/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_PortalFileChooser_h
#define mozilla_widget_PortalFileChooser_h

#include "nsString.h"
#include "nsTArray.h"

#include <functional>

#pragma GCC visibility push(default)
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtDBus/QDBusPendingCallWatcher>
#pragma GCC visibility pop

namespace mozilla::widget {

// xdg-desktop-portal FileChooser over QtDBus (no GIO).
class PortalFileChooser final : public QObject {
  Q_OBJECT
 public:
  // resultCode matches nsIFilePicker ResultCode (returnOK / returnCancel).
  using DoneCallback =
      std::function<void(int32_t resultCode, const QStringList& paths)>;

  explicit PortalFileChooser(DoneCallback aDone);
  ~PortalFileChooser() override;

  // aMode: nsIFilePicker modeOpen / modeOpenMultiple / modeSave / modeGetFolder
  bool Start(int32_t aMode, const nsAString& aTitle, const nsAString& aDefault,
             const nsTArray<nsString>& aFilters,
             const nsTArray<nsString>& aFilterNames);

 private Q_SLOTS:
  void OnOpenFinished(QDBusPendingCallWatcher* aWatcher);
  void OnResponse(uint aResponse, const QVariantMap& aResults);

 private:
  void Finish(int32_t aResultCode, const QStringList& aPaths);
  void DisconnectResponse();
  static QString MakeHandleToken();
  static QString PortalRequestPath(const QString& aToken);

  DoneCallback mDone;
  QString mRequestPath;
  bool mFinished = false;
};

}  // namespace mozilla::widget

#endif
