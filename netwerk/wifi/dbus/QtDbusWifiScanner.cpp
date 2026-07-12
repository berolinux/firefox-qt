/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DbusWifiScanner.h"
#include "nsWifiAccessPoint.h"

#include <cstring>

#pragma GCC visibility push(default)
#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariant>
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusObjectPath>
#include <QtDBus/QDBusVariant>
#pragma GCC visibility pop

namespace mozilla {

namespace {

constexpr const char kNmService[] = "org.freedesktop.NetworkManager";
constexpr const char kNmPath[] = "/org/freedesktop/NetworkManager";
constexpr const char kNmIface[] = "org.freedesktop.NetworkManager";
constexpr const char kDeviceIface[] = "org.freedesktop.NetworkManager.Device";
constexpr const char kWirelessIface[] =
    "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char kApIface[] = "org.freedesktop.NetworkManager.AccessPoint";
constexpr const char kPropsIface[] = "org.freedesktop.DBus.Properties";
// http://projects.gnome.org/NetworkManager/developers/api/07/spec-07.html
constexpr uint32_t kNmDeviceTypeWifi = 2;

QVariant GetProperty(const QString& aPath, const char* aInterface,
                     const char* aProperty) {
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kNmService), aPath, QString::fromLatin1(kPropsIface),
      QStringLiteral("Get"));
  msg << QString::fromLatin1(aInterface) << QString::fromLatin1(aProperty);
  const QDBusMessage reply =
      QDBusConnection::systemBus().call(msg, QDBus::Block, 2000);
  if (reply.type() != QDBusMessage::ReplyMessage ||
      reply.arguments().isEmpty()) {
    return {};
  }
  return reply.arguments().at(0).value<QDBusVariant>().variant();
}

QList<QDBusObjectPath> ObjectPathList(const QVariant& aValue) {
  if (aValue.canConvert<QList<QDBusObjectPath>>()) {
    return aValue.value<QList<QDBusObjectPath>>();
  }
  // Some Qt versions leave ao as QDBusArgument.
  if (aValue.canConvert<QDBusArgument>()) {
    const QDBusArgument arg = aValue.value<QDBusArgument>();
    QList<QDBusObjectPath> paths;
    arg.beginArray();
    while (!arg.atEnd()) {
      QDBusObjectPath path;
      arg >> path;
      paths.append(path);
    }
    arg.endArray();
    return paths;
  }
  return {};
}

}  // namespace

WifiScannerImpl::WifiScannerImpl() { MOZ_COUNT_CTOR(WifiScannerImpl); }

WifiScannerImpl::~WifiScannerImpl() { MOZ_COUNT_DTOR(WifiScannerImpl); }

nsresult WifiScannerImpl::GetAccessPointsFromWLAN(
    AccessPointArray& aAccessPoints) {
  if (!QDBusConnection::systemBus().isConnected()) {
    return NS_ERROR_FAILURE;
  }

  const QVariant devicesVar =
      GetProperty(QString::fromLatin1(kNmPath), kNmIface, "Devices");
  if (!devicesVar.isValid()) {
    return NS_ERROR_FAILURE;
  }

  const QList<QDBusObjectPath> devices = ObjectPathList(devicesVar);
  for (const QDBusObjectPath& device : devices) {
    if (!AddDevice(device.path().toUtf8().constData(), aAccessPoints)) {
      return NS_ERROR_FAILURE;
    }
  }
  return NS_OK;
}

bool WifiScannerImpl::AddDevice(const char* aDevicePath,
                                AccessPointArray& aAccessPoints) {
  if (!aDevicePath || !*aDevicePath) {
    return false;
  }
  const QString path = QString::fromUtf8(aDevicePath);

  const QVariant typeVar = GetProperty(path, kDeviceIface, "DeviceType");
  if (!typeVar.isValid()) {
    return false;
  }
  bool ok = false;
  const uint32_t deviceType = typeVar.toUInt(&ok);
  if (!ok) {
    return false;
  }
  if (deviceType != kNmDeviceTypeWifi) {
    // Don't probe non-wifi devices.
    return true;
  }

  const QVariant apsVar = GetProperty(path, kWirelessIface, "AccessPoints");
  if (!apsVar.isValid()) {
    return false;
  }
  const QList<QDBusObjectPath> aps = ObjectPathList(apsVar);
  for (const QDBusObjectPath& ap : aps) {
    if (!AddAPProperties(ap.path().toUtf8().constData(), aAccessPoints)) {
      return false;
    }
  }
  return true;
}

bool WifiScannerImpl::AddAPProperties(const char* aApPath,
                                      AccessPointArray& aAccessPoints) {
  if (!aApPath || !*aApPath) {
    return false;
  }
  const QString path = QString::fromUtf8(aApPath);

  RefPtr<nsWifiAccessPoint> ap = new nsWifiAccessPoint();

  const QVariant ssidVar = GetProperty(path, kApIface, "Ssid");
  if (!ssidVar.isValid()) {
    return false;
  }
  // NetworkManager exposes SSID as ay (byte array).
  const QByteArray ssid = ssidVar.toByteArray();
  constexpr uint32_t MAX_SSID_LEN = 32;
  if (!ssid.isEmpty() && uint32_t(ssid.size()) <= MAX_SSID_LEN) {
    ap->setSSID(ssid.constData(), ssid.size());
  }

  const QVariant hwVar = GetProperty(path, kApIface, "HwAddress");
  if (!hwVar.isValid()) {
    return false;
  }
  QByteArray address = hwVar.toString().toUtf8();
  if (address.isEmpty() || !SetMac(address.data(), ap)) {
    return false;
  }

  const QVariant strengthVar = GetProperty(path, kApIface, "Strength");
  if (!strengthVar.isValid()) {
    return false;
  }
  // Strength is a byte 0–100 (%). Convert to dBm like the GIO scanner.
  const int strength = strengthVar.toUInt();
  const int signal_strength = (strength / 2) - 100;
  ap->setSignal(signal_strength);

  aAccessPoints.AppendElement(ap);
  return true;
}

bool WifiScannerImpl::SetMac(char* aHwAddress, nsWifiAccessPoint* aAp) {
  // hwAddress format is XX:XX:XX:XX:XX:XX. Need to convert to raw bytes.
  const uint32_t MAC_LEN = 6;
  uint8_t macAddress[MAC_LEN];
  char* savedPtr = nullptr;
  char* token = strtok_r(aHwAddress, ":", &savedPtr);
  for (unsigned char& macAddres : macAddress) {
    if (!token) {
      return false;
    }
    macAddres = strtoul(token, nullptr, 16);
    token = strtok_r(nullptr, ":", &savedPtr);
  }
  aAp->setMac(macAddress);
  return true;
}

}  // namespace mozilla
