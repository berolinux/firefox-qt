/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_QtIncludes_h
#define widget_qt_QtIncludes_h

// Firefox builds with -fvisibility=hidden; Qt headers require default
// visibility on exported types.
#pragma GCC visibility push(default)
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QClipboard>
#include <QMimeData>
#include <QFileDialog>
#include <QColorDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QEventLoop>
#include <QAbstractEventDispatcher>
#include <QFont>
#include <QPalette>
#include <QObject>
#pragma GCC visibility pop

#endif
