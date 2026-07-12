/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "QtEglUtils.h"

#pragma GCC visibility push(default)
#include <QGuiApplication>
#include <QtGui/qguiapplication_platform.h>
#include <qpa/qplatformnativeinterface.h>
#pragma GCC visibility pop

namespace mozilla::widget {

bool QtIsWaylandDisplay() {
  if (!qGuiApp) {
    return false;
  }
  return QGuiApplication::platformName().contains(QLatin1String("wayland"),
                                                  Qt::CaseInsensitive);
}

bool QtIsX11Display() {
  if (!qGuiApp) {
    return false;
  }
  const QString name = QGuiApplication::platformName();
  return name.contains(QLatin1String("xcb"), Qt::CaseInsensitive) ||
         name.contains(QLatin1String("x11"), Qt::CaseInsensitive);
}

void* QtGetWaylandDisplay() {
  if (!QtIsWaylandDisplay() || !qGuiApp) {
    return nullptr;
  }
  if (auto* waylandApp =
          qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
    return waylandApp->display();
  }
  if (QPlatformNativeInterface* native =
          QGuiApplication::platformNativeInterface()) {
    void* d = native->nativeResourceForIntegration("wl_display");
    if (!d) {
      d = native->nativeResourceForIntegration("display");
    }
    return d;
  }
  return nullptr;
}

void* QtGetWaylandCompositor() {
  if (!QtIsWaylandDisplay() || !qGuiApp) {
    return nullptr;
  }
  if (QPlatformNativeInterface* native =
          QGuiApplication::platformNativeInterface()) {
    void* c = native->nativeResourceForIntegration("compositor");
    if (!c) {
      c = native->nativeResourceForIntegration("wl_compositor");
    }
    return c;
  }
  return nullptr;
}

void* QtGetX11Display() {
  if (!QtIsX11Display() || !qGuiApp) {
    return nullptr;
  }
  if (auto* x11App =
          qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
    return x11App->display();
  }
  if (QPlatformNativeInterface* native =
          QGuiApplication::platformNativeInterface()) {
    return native->nativeResourceForIntegration("display");
  }
  return nullptr;
}

}  // namespace mozilla::widget
