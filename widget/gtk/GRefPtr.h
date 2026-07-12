/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GRefPtr_h_
#define GRefPtr_h_

// Allows to use RefPtr<T> with various kinds of GObjects

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#ifdef MOZ_WIDGET_GTK
#  include <gdk/gdk.h>
#  include <gtk/gtk.h>
#endif
#include "mozilla/RefPtr.h"

typedef struct _DbusmenuMenuitem DbusmenuMenuitem;
typedef struct _DbusmenuServer DbusmenuServer;

namespace mozilla {

template <typename T>
struct GObjectRefPtrTraits {
  static void AddRef(T* aObject) { g_object_ref(aObject); }
  static void Release(T* aObject) { g_object_unref(aObject); }
};

#define GOBJECT_TRAITS(type_) \
  template <>                 \
  struct RefPtrTraits<type_> : public GObjectRefPtrTraits<type_> {};

GOBJECT_TRAITS(DbusmenuMenuitem)
GOBJECT_TRAITS(DbusmenuServer)
#ifdef MOZ_WIDGET_GTK
GOBJECT_TRAITS(GtkWidget)
#endif
GOBJECT_TRAITS(GFile)
GOBJECT_TRAITS(GFileMonitor)
GOBJECT_TRAITS(GMenu)
GOBJECT_TRAITS(GMenuItem)
GOBJECT_TRAITS(GSimpleAction)
GOBJECT_TRAITS(GSimpleActionGroup)
GOBJECT_TRAITS(GDBusProxy)
GOBJECT_TRAITS(GAppInfo)
GOBJECT_TRAITS(GDesktopAppInfo)
GOBJECT_TRAITS(GAppLaunchContext)
#ifdef MOZ_WIDGET_GTK
GOBJECT_TRAITS(GdkDragContext)
#endif
GOBJECT_TRAITS(GDBusMessage)
#ifdef MOZ_WIDGET_GTK
GOBJECT_TRAITS(GdkPixbuf)
#endif
GOBJECT_TRAITS(GCancellable)
#ifdef MOZ_WIDGET_GTK
GOBJECT_TRAITS(GtkIMContext)
#endif
GOBJECT_TRAITS(GUnixFDList)
#ifdef MOZ_WIDGET_GTK
GOBJECT_TRAITS(GtkCssProvider)
#endif
GOBJECT_TRAITS(GDBusMethodInvocation)
#ifdef MOZ_WIDGET_GTK
GOBJECT_TRAITS(GdkWindow)
GOBJECT_TRAITS(GtkIconInfo)
#endif
GOBJECT_TRAITS(GIcon)
GOBJECT_TRAITS(::GSettings)
GOBJECT_TRAITS(GDBusConnection)

#undef GOBJECT_TRAITS

template <>
struct RefPtrTraits<GVariant> {
  static void AddRef(GVariant* aVariant) { g_variant_ref(aVariant); }
  static void Release(GVariant* aVariant) { g_variant_unref(aVariant); }
};

template <>
struct RefPtrTraits<GHashTable> {
  static void AddRef(GHashTable* aObject) { g_hash_table_ref(aObject); }
  static void Release(GHashTable* aObject) { g_hash_table_unref(aObject); }
};

template <>
struct RefPtrTraits<GDBusNodeInfo> {
  static void AddRef(GDBusNodeInfo* aObject) { g_dbus_node_info_ref(aObject); }
  static void Release(GDBusNodeInfo* aObject) {
    g_dbus_node_info_unref(aObject);
  }
};

}  // namespace mozilla

#endif
