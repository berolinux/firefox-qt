/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_QtEglUtils_h
#define widget_qt_QtEglUtils_h

namespace mozilla::widget {

// True when QPA is the Wayland backend.
bool QtIsWaylandDisplay();

// True when QPA is the X11/xcb backend.
bool QtIsX11Display();

// wl_display* for EGL on Wayland, or nullptr.
void* QtGetWaylandDisplay();

// wl_compositor* for creating offscreen surfaces on Wayland, or nullptr.
void* QtGetWaylandCompositor();

// X11 Display* for EGL on xcb, or nullptr.
void* QtGetX11Display();

}  // namespace mozilla::widget

#endif
