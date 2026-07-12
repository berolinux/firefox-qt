/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DBusMenuBar.h"
#include "MenuUtilsQt.h"

#include "mozilla/BasicEvents.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/Logging.h"
#include "QtPrefUtils.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/XULCommandEvent.h"
#include "nsContentUtils.h"
#include "nsGlobalWindowInner.h"
#include "nsIWidget.h"
#include "nsPrintfCString.h"
#include "nsStubMutationObserver.h"
#include "nsWindow.h"
#include "QtWindowBridge.h"

#pragma GCC visibility push(default)
#include <QGuiApplication>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>
#pragma GCC visibility pop

#include <wayland-client.h>
#include "mozilla/widget/kde-appmenu-client-protocol.h"

// QtDBus used by both backends for AppMenu.Registrar (no GIO).
#pragma GCC visibility push(default)
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusReply>
#pragma GCC visibility pop

// Preferred: libdbusmenu-qt6
#ifdef MOZ_ENABLE_DBUSMENU_QT
#  pragma GCC visibility push(default)
#  include <QAction>
#  include <QMenu>
#  include <dbusmenuexporter.h>
#  pragma GCC visibility pop
#endif

// Optional fallback: libdbusmenu-glib (GLib/GObject only — no GIO).
#include "DBusMenu.h"

namespace mozilla::widget {

static LazyLogModule gQtGlobalMenuLog("QtGlobalMenu");

// ---------------------------------------------------------------------------
// Shared XUL helpers
// ---------------------------------------------------------------------------

static bool IsDisabled(const dom::Element& aElement) {
  return aElement.GetBoolAttr(nsGkAtoms::disabled) ||
         aElement.GetBoolAttr(nsGkAtoms::hidden);
}

static bool NodeIsRelevant(const nsINode& aNode) {
  return aNode.IsAnyOfXULElements(nsGkAtoms::menu, nsGkAtoms::menuseparator,
                                  nsGkAtoms::menuitem, nsGkAtoms::menugroup,
                                  nsGkAtoms::menubar);
}

static Maybe<bool> GetChecked(const dom::Element& aMenuItem) {
  static dom::Element::AttrValuesArray strings[] = {nsGkAtoms::checkbox,
                                                    nsGkAtoms::radio, nullptr};
  switch (aMenuItem.FindAttrValueIn(kNameSpaceID_None, nsGkAtoms::type, strings,
                                    eCaseMatters)) {
    case 0:
    case 1:
      break;
    default:
      return Nothing();
  }
  return Some(aMenuItem.GetBoolAttr(nsGkAtoms::checked));
}

static MOZ_CAN_RUN_SCRIPT void ActivateItem(dom::Element& aElement) {
  if (Maybe<bool> checked = GetChecked(aElement)) {
    if (!aElement.AttrValueIs(kNameSpaceID_None, nsGkAtoms::autocheck,
                              nsGkAtoms::_false, eCaseMatters)) {
      bool newValue = !*checked;
      if (newValue) {
        aElement.SetAttr(kNameSpaceID_None, nsGkAtoms::checked, u"true"_ns,
                         true);
      } else {
        aElement.UnsetAttr(kNameSpaceID_None, nsGkAtoms::checked, true);
      }
    }
  }

  RefPtr doc = aElement.OwnerDoc();
  RefPtr event = new dom::XULCommandEvent(doc, doc->GetPresContext(), nullptr);
  IgnoredErrorResult rv;
  event->InitCommandEvent(u"command"_ns, true, true,
                          nsGlobalWindowInner::Cast(doc->GetInnerWindow()), 0,
                          false, false, false, false, MouseButton::ePrimary,
                          nullptr, 0, rv);
  if (MOZ_UNLIKELY(rv.Failed())) {
    return;
  }
  aElement.DispatchEvent(*event);
}

static MOZ_CAN_RUN_SCRIPT void FireEvent(dom::Element* aTarget,
                                         EventMessage aPopupMessage) {
  nsEventStatus status = nsEventStatus_eIgnore;
  WidgetMouseEvent event(true, aPopupMessage, nullptr, WidgetMouseEvent::eReal);
  EventDispatcher::Dispatch(aTarget, nullptr, &event, nullptr, &status);
}

static const dom::Element* GetMenuPopupChild(const dom::Element& aElement) {
  for (const nsIContent* child = aElement.GetFirstChild(); child;
       child = child->GetNextSibling()) {
    if (child->IsXULElement(nsGkAtoms::menupopup)) {
      return child->AsElement();
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Mutation observer base
// ---------------------------------------------------------------------------

class MenuModelBase : public nsStubMutationObserver {
  NS_DECL_ISUPPORTS
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTREMOVED
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTINSERTED
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTAPPENDED
  NS_DECL_NSIMUTATIONOBSERVER_ATTRIBUTECHANGED

 public:
  explicit MenuModelBase(dom::Element* aElement) : mElement(aElement) {
    mElement->AddMutationObserver(this);
  }

  dom::Element* Element() { return mElement; }

  void RecomputeModelIfNeeded() {
    if (!mDirty) {
      return;
    }
    RecomputeModel();
    mDirty = false;
  }

 protected:
  virtual void RecomputeModel() = 0;
  virtual ~MenuModelBase() { mElement->RemoveMutationObserver(this); }

  void DirtyModel() {
    mDirty = true;
    if (mShowing) {
      RecomputeModelIfNeeded();
    }
  }

  RefPtr<dom::Element> mElement;
  bool mDirty = true;
  bool mShowing = false;
};

NS_IMPL_ISUPPORTS(MenuModelBase, nsIMutationObserver)

void MenuModelBase::ContentWillBeRemoved(nsIContent* aChild,
                                         const ContentRemoveInfo&) {
  if (NodeIsRelevant(*aChild)) {
    nsContentUtils::AddScriptRunner(
        NewRunnableMethod("MenuModelBase::ContentWillBeRemoved", this,
                          &MenuModelBase::DirtyModel));
  }
}

void MenuModelBase::ContentInserted(nsIContent* aChild,
                                    const ContentInsertInfo&) {
  if (NodeIsRelevant(*aChild)) {
    DirtyModel();
  }
}

void MenuModelBase::ContentAppended(nsIContent* aChild,
                                    const ContentAppendInfo&) {
  if (NodeIsRelevant(*aChild)) {
    DirtyModel();
  }
}

void MenuModelBase::AttributeChanged(dom::Element* aElement, int32_t,
                                     nsAtom* aAttribute, AttrModType,
                                     const nsAttrValue*) {
  if (NodeIsRelevant(*aElement) &&
      (aAttribute == nsGkAtoms::label || aAttribute == nsGkAtoms::aria_label ||
       aAttribute == nsGkAtoms::disabled || aAttribute == nsGkAtoms::hidden ||
       aAttribute == nsGkAtoms::checked)) {
    DirtyModel();
  }
}

// ---------------------------------------------------------------------------
// Wayland appmenu global
// ---------------------------------------------------------------------------

static org_kde_kwin_appmenu_manager* sAppMenuManager = nullptr;
static bool sAppMenuManagerTried = false;

static void AppMenuRegistryGlobal(void*, wl_registry* aRegistry, uint32_t aId,
                                  const char* aInterface, uint32_t aVersion) {
  if (strcmp(aInterface, "org_kde_kwin_appmenu_manager") == 0) {
    sAppMenuManager = static_cast<org_kde_kwin_appmenu_manager*>(
        wl_registry_bind(aRegistry, aId,
                         &org_kde_kwin_appmenu_manager_interface,
                         std::min(aVersion, 2u)));
  }
}

static void AppMenuRegistryGlobalRemove(void*, wl_registry*, uint32_t) {}

static const wl_registry_listener kAppMenuRegistryListener = {
    AppMenuRegistryGlobal,
    AppMenuRegistryGlobalRemove,
};

static wl_display* QtWlDisplay() {
  QPlatformNativeInterface* native =
      QGuiApplication::platformNativeInterface();
  if (!native) {
    return nullptr;
  }
  return static_cast<wl_display*>(
      native->nativeResourceForIntegration("wl_display"));
}

static wl_surface* QtWlSurface(QWindow* aWindow) {
  if (!aWindow) {
    return nullptr;
  }
  if (!aWindow->handle()) {
    aWindow->create();
  }
  QPlatformNativeInterface* native =
      QGuiApplication::platformNativeInterface();
  if (!native) {
    return nullptr;
  }
  return static_cast<wl_surface*>(
      native->nativeResourceForWindow("surface", aWindow));
}

static bool EnsureAppMenuManager() {
  if (sAppMenuManagerTried) {
    return !!sAppMenuManager;
  }
  sAppMenuManagerTried = true;

  if (!QGuiApplication::platformName().contains(QLatin1String("wayland"),
                                                Qt::CaseInsensitive)) {
    return false;
  }

  wl_display* display = QtWlDisplay();
  if (!display) {
    return false;
  }

  wl_registry* registry = wl_display_get_registry(display);
  if (!registry) {
    return false;
  }
  wl_registry_add_listener(registry, &kAppMenuRegistryListener, nullptr);
  wl_display_roundtrip(display);

  MOZ_LOG(gQtGlobalMenuLog, LogLevel::Info,
          ("org_kde_kwin_appmenu_manager %s",
           sAppMenuManager ? "available" : "missing"));
  return !!sAppMenuManager;
}

/* static */
bool DBusMenuBar::WaylandAppMenuAvailable() {
  return EnsureAppMenuManager();
}

// ---------------------------------------------------------------------------
// Shared registration helpers
// ---------------------------------------------------------------------------

static QWindow* WindowForElement(dom::Element* aElement) {
  RefPtr win = aElement->OwnerDoc()->GetInnerWindow();
  if (NS_WARN_IF(!win)) {
    return nullptr;
  }
  nsIWidget* widget = nsGlobalWindowInner::Cast(win.get())->GetNearestWidget();
  if (NS_WARN_IF(!widget)) {
    return nullptr;
  }
  auto* nsWin = nsWindow::FromWidget(widget);
  if (NS_WARN_IF(!nsWin) || !nsWin->GetQtWindow()) {
    return nullptr;
  }
  return nsWin->GetQtWindow();
}

static void SetNativeAttr(dom::Element* aElement, bool aNative) {
  if (aElement) {
    aElement->SetBoolAttr(nsGkAtoms::native, aNative);
  }
}

// Registers the dbusmenu object path with Plasma. aServiceName is the unique
// bus name that owns the menu object (must match the exporter connection).
static void RegisterAppMenuWayland(org_kde_kwin_appmenu** aAppMenuSlot,
                                   QWindow* aWindow, const char* aServiceName,
                                   const nsACString& aObjectPath,
                                   dom::Element* aElement) {
  if (!PrefGlobalMenuWaylandEnabled()) {
    return;
  }
  if (!EnsureAppMenuManager()) {
    MOZ_LOG(gQtGlobalMenuLog, LogLevel::Warning,
            ("Wayland global menu enabled but no appmenu manager"));
    return;
  }
  wl_surface* surface = QtWlSurface(aWindow);
  if (NS_WARN_IF(!surface) || NS_WARN_IF(!aServiceName)) {
    return;
  }
  if (!*aAppMenuSlot) {
    *aAppMenuSlot =
        org_kde_kwin_appmenu_manager_create(sAppMenuManager, surface);
  }
  if (NS_WARN_IF(!*aAppMenuSlot)) {
    return;
  }
  SetNativeAttr(aElement, true);
  org_kde_kwin_appmenu_set_address(*aAppMenuSlot, aServiceName,
                                   PromiseFlatCString(aObjectPath).get());
  MOZ_LOG(gQtGlobalMenuLog, LogLevel::Info,
          ("Wayland appmenu set_address %s %s", aServiceName,
           PromiseFlatCString(aObjectPath).get()));
}

// ---------------------------------------------------------------------------
// Backend interface
// ---------------------------------------------------------------------------

class DBusMenuBar::Backend {
 public:
  virtual ~Backend() = default;
  virtual dom::Element* Element() = 0;
  virtual const nsCString& ObjectPath() const = 0;
  // Unique bus name that owns the menu object path.
  virtual nsCString ServiceName() const = 0;
  // Called when AppMenu.Registrar (or Wayland) is ready for registration.
  virtual void OnRegistrarReady() {}

  void SetOwner(DBusMenuBar* aOwner) { mOwner = aOwner; }

 protected:
  DBusMenuBar* mOwner = nullptr;  // non-owning
};

// ---------------------------------------------------------------------------
// Qt backend (preferred): libdbusmenu-qt6
// ---------------------------------------------------------------------------

#ifdef MOZ_ENABLE_DBUSMENU_QT

class MenubarModelQt final : public MenuModelBase {
 public:
  explicit MenubarModelQt(dom::Element* aElement) : MenuModelBase(aElement) {
    mRootMenu = new QMenu();
    mShowing = true;
  }

  QMenu* RootMenu() const { return mRootMenu; }

 protected:
  ~MenubarModelQt() override { delete mRootMenu; }

  void RecomputeModel() override {
    mRootMenu->clear();
    RecomputeModelFor(mRootMenu, *Element());
  }

  static void AppendMenuItem(QMenu* aParent, const dom::Element* aChild) {
    nsAutoString label;
    aChild->GetAttr(nsGkAtoms::label, label);
    if (label.IsEmpty()) {
      aChild->GetAttr(nsGkAtoms::aria_label, label);
    }
    QAction* action =
        aParent->addAction(QString::fromUtf16(label.BeginReading(),
                                              int(label.Length())));
    action->setEnabled(!aChild->GetBoolAttr(nsGkAtoms::disabled));
    if (Maybe<bool> checked = GetChecked(*aChild)) {
      action->setCheckable(true);
      action->setChecked(*checked);
    }
    RefPtr element = const_cast<dom::Element*>(aChild);
    ApplyMenuShortcuts(action, element);
    ApplyMenuIcon(action, element);
    QObject::connect(action, &QAction::triggered, action, [element]() {
      ActivateItem(*element);
    });
  }

  static void AppendSeparator(QMenu* aParent) { aParent->addSeparator(); }

  static void AppendSubmenu(QMenu* aParent, const dom::Element* aMenu,
                            const dom::Element* aPopup) {
    nsAutoString label;
    aMenu->GetAttr(nsGkAtoms::label, label);
    QMenu* sub =
        aParent->addMenu(QString::fromUtf16(label.BeginReading(),
                                            int(label.Length())));
    RefPtr popup = const_cast<dom::Element*>(aPopup);
    QObject::connect(sub, &QMenu::aboutToShow, sub, [popup]() {
      FireEvent(popup, eXULPopupShowing);
      FireEvent(popup, eXULPopupShown);
    });
    QObject::connect(sub, &QMenu::aboutToHide, sub, [popup]() {
      FireEvent(popup, eXULPopupHiding);
      FireEvent(popup, eXULPopupHidden);
    });
    if (RecomputeModelFor(sub, *aPopup) == 0) {
      // dbusmenu clients expect at least a placeholder for empty submenus.
      sub->addAction(QString())->setEnabled(false);
    }
  }

  static uint RecomputeModelFor(QMenu* aParent, const dom::Element& aElement) {
    uint childCount = 0;
    for (const nsIContent* child = aElement.GetFirstChild(); child;
         child = child->GetNextSibling()) {
      if (child->IsXULElement(nsGkAtoms::menuitem) &&
          !IsDisabled(*child->AsElement())) {
        AppendMenuItem(aParent, child->AsElement());
        childCount++;
        continue;
      }
      if (child->IsXULElement(nsGkAtoms::menuseparator)) {
        AppendSeparator(aParent);
        childCount++;
        continue;
      }
      if (child->IsXULElement(nsGkAtoms::menu) &&
          !IsDisabled(*child->AsElement())) {
        if (const auto* popup = GetMenuPopupChild(*child->AsElement())) {
          childCount++;
          AppendSubmenu(aParent, child->AsElement(), popup);
        }
      }
    }
    return childCount;
  }

  QMenu* mRootMenu = nullptr;
};

class DBusMenuBar::QtBackend final : public DBusMenuBar::Backend {
 public:
  explicit QtBackend(dom::Element* aElement)
      : mObjectPath(nsPrintfCString("/com/canonical/menu/%u", sID++)),
        mModel(MakeRefPtr<MenubarModelQt>(aElement)) {
    mModel->RecomputeModelIfNeeded();
    mExporter = mozilla::MakeUnique<DBusMenuExporter>(
        QString::fromUtf8(mObjectPath.get()), mModel->RootMenu(),
        QDBusConnection::sessionBus());

    // Watch AppMenu.Registrar appearing on the session bus.
    mWatcher = mozilla::MakeUnique<QDBusServiceWatcher>(
        QStringLiteral("com.canonical.AppMenu.Registrar"),
        QDBusConnection::sessionBus(),
        QDBusServiceWatcher::WatchForOwnerChange);
    QObject::connect(mWatcher.get(),
                     &QDBusServiceWatcher::serviceOwnerChanged, mWatcher.get(),
                     [this](const QString&, const QString&,
                            const QString& newOwner) {
                       if (!newOwner.isEmpty()) {
                         OnRegistrarReady();
                       }
                     });
    // If already present, register immediately.
    if (QDBusConnection::sessionBus().interface()->isServiceRegistered(
            QStringLiteral("com.canonical.AppMenu.Registrar"))) {
      OnRegistrarReady();
    }
  }

  dom::Element* Element() override { return mModel->Element(); }
  const nsCString& ObjectPath() const override { return mObjectPath; }

  nsCString ServiceName() const override {
    nsCString name;
    name.Assign(
        QDBusConnection::sessionBus().baseService().toUtf8().constData());
    return name;
  }

  void OnRegistrarReady() override {
    QWindow* qWin = WindowForElement(mModel->Element());
    if (!qWin) {
      return;
    }
    const bool isWayland = QGuiApplication::platformName().contains(
        QLatin1String("wayland"), Qt::CaseInsensitive);
    if (isWayland) {
      if (mOwner) {
        mOwner->TryRegisterWayland();
      }
      return;
    }
    if (!qWin->handle()) {
      qWin->create();
    }
    const quint32 xid = static_cast<quint32>(qWin->winId());
    if (!xid) {
      return;
    }
    QDBusInterface iface(
        QStringLiteral("com.canonical.AppMenu.Registrar"),
        QStringLiteral("/com/canonical/AppMenu/Registrar"),
        QStringLiteral("com.canonical.AppMenu.Registrar"),
        QDBusConnection::sessionBus());
    if (!iface.isValid()) {
      MOZ_LOG(gQtGlobalMenuLog, LogLevel::Warning,
              ("AppMenu.Registrar interface invalid: %s",
               qPrintable(iface.lastError().message())));
      return;
    }
    QDBusPendingCall call =
        iface.asyncCall(QStringLiteral("RegisterWindow"), xid,
                        QString::fromUtf8(mObjectPath.get()));
    auto* watcher = new QDBusPendingCallWatcher(call);
    QObject::connect(
        watcher, &QDBusPendingCallWatcher::finished, watcher,
        [element = RefPtr{mModel->Element()}, path = mObjectPath,
         watcher](QDBusPendingCallWatcher* w) {
          QDBusPendingReply<> reply = *w;
          if (reply.isError()) {
            MOZ_LOG(gQtGlobalMenuLog, LogLevel::Warning,
                    ("RegisterWindow failed: %s",
                     qPrintable(reply.error().message())));
            SetNativeAttr(element, false);
          } else {
            MOZ_LOG(gQtGlobalMenuLog, LogLevel::Info,
                    ("RegisterWindow ok path=%s", path.get()));
            SetNativeAttr(element, true);
          }
          w->deleteLater();
        });
  }

 private:
  static unsigned sID;
  nsCString mObjectPath;
  RefPtr<MenubarModelQt> mModel;
  mozilla::UniquePtr<DBusMenuExporter> mExporter;
  mozilla::UniquePtr<QDBusServiceWatcher> mWatcher;
};

unsigned DBusMenuBar::QtBackend::sID = 0;

#endif  // MOZ_ENABLE_DBUSMENU_QT

// ---------------------------------------------------------------------------
// Glib backend (optional fallback): libdbusmenu-glib via dlopen
// ---------------------------------------------------------------------------

class MenubarModelGlib final : public MenuModelBase {
 public:
  explicit MenubarModelGlib(dom::Element* aElement) : MenuModelBase(aElement) {
    mRoot = dont_AddRef(dbusmenu_menuitem_new());
    dbusmenu_menuitem_set_root(mRoot.get(), true);
    mShowing = true;
  }

  DbusmenuMenuitem* Root() const { return mRoot.get(); }

 protected:
  void RecomputeModel() override {
    while (GList* children = dbusmenu_menuitem_get_children(mRoot.get())) {
      auto* first = static_cast<DbusmenuMenuitem*>(children->data);
      if (!first) {
        break;
      }
      dbusmenu_menuitem_child_delete(mRoot.get(), first);
    }
    RecomputeModelFor(mRoot, *Element());
  }

  static void UpdateRadioOrCheck(DbusmenuMenuitem* aItem,
                                 const dom::Element* aContent) {
    static mozilla::dom::Element::AttrValuesArray attrs[] = {
        nsGkAtoms::checkbox, nsGkAtoms::radio, nullptr};
    int32_t type = aContent->FindAttrValueIn(kNameSpaceID_None, nsGkAtoms::type,
                                             attrs, eCaseMatters);
    if (type < 0 || type >= 2) {
      return;
    }
    if (type == 0) {
      dbusmenu_menuitem_property_set(aItem, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
                                     DBUSMENU_MENUITEM_TOGGLE_CHECK);
    } else {
      dbusmenu_menuitem_property_set(aItem, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
                                     DBUSMENU_MENUITEM_TOGGLE_RADIO);
    }
    const bool isChecked = aContent->GetBoolAttr(nsGkAtoms::checked);
    dbusmenu_menuitem_property_set_int(
        aItem, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
        isChecked ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                  : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
  }

  static void UpdateEnabled(DbusmenuMenuitem* aItem,
                            const nsIContent* aContent) {
    const bool disabled =
        aContent->AsElement()->GetBoolAttr(nsGkAtoms::disabled);
    dbusmenu_menuitem_property_set_bool(aItem, DBUSMENU_MENUITEM_PROP_ENABLED,
                                        !disabled);
  }

  static MOZ_CAN_RUN_SCRIPT void DBusActivationCallback(
      DbusmenuMenuitem*, guint, gpointer aUserData) {
    RefPtr element = static_cast<dom::Element*>(aUserData);
    ActivateItem(*element);
  }

  static void ConnectActivated(DbusmenuMenuitem* aItem,
                               const dom::Element* aContent) {
    g_signal_connect(G_OBJECT(aItem), DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                     G_CALLBACK(DBusActivationCallback),
                     const_cast<dom::Element*>(aContent));
  }

  static MOZ_CAN_RUN_SCRIPT bool DBusEventCallback(DbusmenuMenuitem*,
                                                   const gchar* name, GVariant*,
                                                   guint, gpointer aUserData) {
    RefPtr element = static_cast<dom::Element*>(aUserData);
    if (strcmp(name, "opened") == 0) {
      FireEvent(element, eXULPopupShowing);
      FireEvent(element, eXULPopupShown);
    } else if (strcmp(name, "closed") == 0) {
      FireEvent(element, eXULPopupHiding);
      FireEvent(element, eXULPopupHidden);
    }
    return false;
  }

  static void ConnectEvent(DbusmenuMenuitem* aItem,
                           const dom::Element* aContent) {
    g_signal_connect(G_OBJECT(aItem), DBUSMENU_MENUITEM_SIGNAL_EVENT,
                     G_CALLBACK(DBusEventCallback),
                     const_cast<dom::Element*>(aContent));
  }

  static void AppendMenuItem(DbusmenuMenuitem* aParent,
                             const dom::Element* aChild) {
    nsAutoString label;
    aChild->GetAttr(nsGkAtoms::label, label);
    if (label.IsEmpty()) {
      aChild->GetAttr(nsGkAtoms::aria_label, label);
    }
    RefPtr<DbusmenuMenuitem> child = dont_AddRef(dbusmenu_menuitem_new());
    dbusmenu_menuitem_property_set(child, DBUSMENU_MENUITEM_PROP_LABEL,
                                   NS_ConvertUTF16toUTF8(label).get());
    dbusmenu_menuitem_child_append(aParent, child);
    UpdateRadioOrCheck(child, aChild);
    UpdateEnabled(child, aChild);
    ConnectActivated(child, aChild);
  }

  static void AppendSeparator(DbusmenuMenuitem* aParent) {
    RefPtr<DbusmenuMenuitem> child = dont_AddRef(dbusmenu_menuitem_new());
    dbusmenu_menuitem_property_set(child, DBUSMENU_MENUITEM_PROP_TYPE,
                                   "separator");
    dbusmenu_menuitem_child_append(aParent, child);
  }

  static void AppendSubmenu(DbusmenuMenuitem* aParent,
                            const dom::Element* aMenu,
                            const dom::Element* aPopup) {
    RefPtr<DbusmenuMenuitem> submenu = dont_AddRef(dbusmenu_menuitem_new());
    if (RecomputeModelFor(submenu, *aPopup) == 0) {
      RefPtr<DbusmenuMenuitem> placeholder =
          dont_AddRef(dbusmenu_menuitem_new());
      dbusmenu_menuitem_child_append(submenu, placeholder);
    }
    nsAutoString label;
    aMenu->GetAttr(nsGkAtoms::label, label);
    ConnectEvent(submenu, aPopup);
    dbusmenu_menuitem_property_set(submenu, DBUSMENU_MENUITEM_PROP_LABEL,
                                   NS_ConvertUTF16toUTF8(label).get());
    dbusmenu_menuitem_child_append(aParent, submenu);
  }

  static uint RecomputeModelFor(DbusmenuMenuitem* aParent,
                                const dom::Element& aElement) {
    uint childCount = 0;
    for (const nsIContent* child = aElement.GetFirstChild(); child;
         child = child->GetNextSibling()) {
      if (child->IsXULElement(nsGkAtoms::menuitem) &&
          !IsDisabled(*child->AsElement())) {
        AppendMenuItem(aParent, child->AsElement());
        childCount++;
        continue;
      }
      if (child->IsXULElement(nsGkAtoms::menuseparator)) {
        AppendSeparator(aParent);
        childCount++;
        continue;
      }
      if (child->IsXULElement(nsGkAtoms::menu) &&
          !IsDisabled(*child->AsElement())) {
        if (const auto* popup = GetMenuPopupChild(*child->AsElement())) {
          childCount++;
          AppendSubmenu(aParent, child->AsElement(), popup);
        }
      }
    }
    return childCount;
  }

  RefPtr<DbusmenuMenuitem> mRoot;
};

class DBusMenuBar::GlibBackend final : public DBusMenuBar::Backend {
 public:
  explicit GlibBackend(dom::Element* aElement)
      : mObjectPath(nsPrintfCString("/com/canonical/menu/%u", sID++)),
        mModel(MakeRefPtr<MenubarModelGlib>(aElement)),
        mServer(dont_AddRef(dbusmenu_server_new(mObjectPath.get()))) {
    mModel->RecomputeModelIfNeeded();
    dbusmenu_server_set_root(mServer.get(), mModel->Root());

    // AppMenu.Registrar via QtDBus (no GIO).
    mWatcher = mozilla::MakeUnique<QDBusServiceWatcher>(
        QStringLiteral("com.canonical.AppMenu.Registrar"),
        QDBusConnection::sessionBus(),
        QDBusServiceWatcher::WatchForOwnerChange);
    QObject::connect(mWatcher.get(),
                     &QDBusServiceWatcher::serviceOwnerChanged, mWatcher.get(),
                     [this](const QString&, const QString&,
                            const QString& newOwner) {
                       if (!newOwner.isEmpty()) {
                         OnRegistrarReady();
                       }
                     });
    if (QDBusConnection::sessionBus().interface()->isServiceRegistered(
            QStringLiteral("com.canonical.AppMenu.Registrar"))) {
      OnRegistrarReady();
    }
  }

  dom::Element* Element() override { return mModel->Element(); }
  const nsCString& ObjectPath() const override { return mObjectPath; }

  nsCString ServiceName() const override {
    nsCString name;
    name.Assign(
        QDBusConnection::sessionBus().baseService().toUtf8().constData());
    return name;
  }

  void OnRegistrarReady() override {
    QWindow* qWin = WindowForElement(mModel->Element());
    if (!qWin) {
      return;
    }
    const bool isWayland = QGuiApplication::platformName().contains(
        QLatin1String("wayland"), Qt::CaseInsensitive);
    if (isWayland) {
      if (mOwner) {
        mOwner->TryRegisterWayland();
      }
      return;
    }
    if (!qWin->handle()) {
      qWin->create();
    }
    const quint32 xid = static_cast<quint32>(qWin->winId());
    if (!xid) {
      return;
    }
    QDBusInterface iface(
        QStringLiteral("com.canonical.AppMenu.Registrar"),
        QStringLiteral("/com/canonical/AppMenu/Registrar"),
        QStringLiteral("com.canonical.AppMenu.Registrar"),
        QDBusConnection::sessionBus());
    if (!iface.isValid()) {
      MOZ_LOG(gQtGlobalMenuLog, LogLevel::Warning,
              ("AppMenu.Registrar interface invalid: %s",
               qPrintable(iface.lastError().message())));
      return;
    }
    QDBusPendingCall call =
        iface.asyncCall(QStringLiteral("RegisterWindow"), xid,
                        QString::fromUtf8(mObjectPath.get()));
    auto* watcher = new QDBusPendingCallWatcher(call);
    QObject::connect(
        watcher, &QDBusPendingCallWatcher::finished, watcher,
        [element = RefPtr{mModel->Element()}, path = mObjectPath,
         watcher](QDBusPendingCallWatcher* w) {
          QDBusPendingReply<> reply = *w;
          if (reply.isError()) {
            MOZ_LOG(gQtGlobalMenuLog, LogLevel::Warning,
                    ("RegisterWindow failed: %s",
                     qPrintable(reply.error().message())));
            SetNativeAttr(element, false);
          } else {
            MOZ_LOG(gQtGlobalMenuLog, LogLevel::Info,
                    ("RegisterWindow ok path=%s", path.get()));
            SetNativeAttr(element, true);
          }
          w->deleteLater();
        });
  }

 private:
  static unsigned sID;
  nsCString mObjectPath;
  RefPtr<MenubarModelGlib> mModel;
  RefPtr<DbusmenuServer> mServer;
  mozilla::UniquePtr<QDBusServiceWatcher> mWatcher;
};

unsigned DBusMenuBar::GlibBackend::sID = 0;

// ---------------------------------------------------------------------------
// DBusMenuBar public API
// ---------------------------------------------------------------------------

/* static */
bool DBusMenuBar::Available() {
#ifdef MOZ_ENABLE_DBUSMENU_QT
  return true;
#else
  return DBusMenuFunctions::Init();
#endif
}

/* static */
RefPtr<DBusMenuBar> DBusMenuBar::Create(dom::Element* aElement) {
  UniquePtr<Backend> backend;

  // Prefer libdbusmenu-qt6 when built with it.
#ifdef MOZ_ENABLE_DBUSMENU_QT
  MOZ_LOG(gQtGlobalMenuLog, LogLevel::Info,
          ("Using libdbusmenu-qt6 backend"));
  backend = MakeUnique<QtBackend>(aElement);
#endif

  // Optional libdbusmenu-glib fallback (dlopen) if Qt backend unavailable.
  if (!backend) {
    if (!DBusMenuFunctions::Init()) {
      MOZ_LOG(gQtGlobalMenuLog, LogLevel::Warning,
              ("No dbusmenu backend available (install libdbusmenu-qt6 "
               "or libdbusmenu-glib)"));
      return nullptr;
    }
    MOZ_LOG(gQtGlobalMenuLog, LogLevel::Info,
            ("Using libdbusmenu-glib fallback backend"));
    backend = MakeUnique<GlibBackend>(aElement);
  }

  RefPtr<DBusMenuBar> self = new DBusMenuBar(std::move(backend));
  self->mBackend->SetOwner(self.get());
  self->TryRegisterWayland();
  return self;
}

void DBusMenuBar::TryRegisterWayland() {
  if (!mBackend) {
    return;
  }
  QWindow* qWin = WindowForElement(mBackend->Element());
  if (!qWin || !QGuiApplication::platformName().contains(
                   QLatin1String("wayland"), Qt::CaseInsensitive)) {
    return;
  }
  nsCString service = mBackend->ServiceName();
  if (service.IsEmpty()) {
    return;
  }
  RegisterAppMenuWayland(&mAppMenu, qWin, service.get(),
                         mBackend->ObjectPath(), mBackend->Element());
}

DBusMenuBar::DBusMenuBar(UniquePtr<Backend> aBackend)
    : mBackend(std::move(aBackend)) {}

DBusMenuBar::~DBusMenuBar() {
  if (mAppMenu) {
    org_kde_kwin_appmenu_release(mAppMenu);
    mAppMenu = nullptr;
  }
}

}  // namespace mozilla::widget
