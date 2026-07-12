/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MenuUtilsQt.h"

#include "mozilla/WidgetUtils.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"
#include "mozilla/widget/NativeMenu.h"
#include "nsIFile.h"
#include "nsIFileURL.h"
#include "nsNetUtil.h"
#include "nsCRT.h"
#include "nsString.h"

#include <cstring>

#pragma GCC visibility push(default)
#include <QAction>
#include <QIcon>
#include <QKeySequence>
#pragma GCC visibility pop

namespace mozilla::widget {

static const dom::Element* RelevantElementForKeys(
    const dom::Element* aElement) {
  nsAutoString key;
  aElement->GetAttr(nsGkAtoms::key, key);
  if (!key.IsEmpty()) {
    if (dom::Element* el = aElement->OwnerDoc()->GetElementById(key)) {
      return el;
    }
  }
  return aElement;
}

static QKeySequence KeySequenceFrom(const dom::Element* aElement) {
  const auto* element = RelevantElementForKeys(aElement);
  nsAutoString key;
  nsAutoString keycode;
  element->GetAttr(nsGkAtoms::key, key);
  element->GetAttr(nsGkAtoms::keycode, keycode);

  QString keyPart;
  if (!key.IsEmpty()) {
    keyPart =
        QString::fromUtf16(key.BeginReading(), int(key.Length())).toUpper();
  } else if (!keycode.IsEmpty()) {
    NS_ConvertUTF16toUTF8 code(keycode);
    if (StringBeginsWith(code, "VK_"_ns)) {
      keyPart = QString::fromUtf8(code.get() + 3);
    } else {
      keyPart = QString::fromUtf8(code.get());
    }
  }
  if (keyPart.isEmpty()) {
    return {};
  }

  QStringList parts;
  nsAutoString modifiers;
  element->GetAttr(nsGkAtoms::modifiers, modifiers);
  if (!modifiers.IsEmpty()) {
    char* str = ToNewUTF8String(modifiers);
    char* token = strtok(str, ", \t");
    while (token) {
      if (nsCRT::strcmp(token, "shift") == 0) {
        parts << QStringLiteral("Shift");
      } else if (nsCRT::strcmp(token, "alt") == 0) {
        parts << QStringLiteral("Alt");
      } else if (nsCRT::strcmp(token, "meta") == 0) {
        parts << QStringLiteral("Meta");
      } else if (nsCRT::strcmp(token, "control") == 0 ||
                 nsCRT::strcmp(token, "accel") == 0) {
        parts << QStringLiteral("Ctrl");
      }
      token = strtok(nullptr, ", \t");
    }
    free(str);
  }
  parts << keyPart;
  return QKeySequence(parts.join(QLatin1Char('+')));
}

void ApplyMenuShortcuts(QAction* aAction, const dom::Element* aElement) {
  if (!aAction || !aElement) {
    return;
  }
  QKeySequence seq = KeySequenceFrom(aElement);
  if (!seq.isEmpty()) {
    aAction->setShortcut(seq);
  }
}

void ApplyMenuIcon(QAction* aAction, dom::Element* aElement) {
  if (!aAction || !aElement) {
    return;
  }
  NativeMenuIcon icon = NativeMenu::GetIcon(*aElement);
  if (!icon || !icon.mURI) {
    return;
  }
  nsAutoCString scheme;
  icon.mURI->GetScheme(scheme);
  if (scheme.EqualsLiteral("file")) {
    nsCOMPtr<nsIFileURL> fileUrl = do_QueryInterface(icon.mURI);
    if (!fileUrl) {
      return;
    }
    nsCOMPtr<nsIFile> file;
    if (NS_FAILED(fileUrl->GetFile(getter_AddRefs(file))) || !file) {
      return;
    }
    nsAutoCString path;
    if (NS_FAILED(file->GetNativePath(path))) {
      return;
    }
    QIcon qicon(QString::fromUtf8(path.get()));
    if (!qicon.isNull()) {
      aAction->setIcon(qicon);
    }
    return;
  }
  // Theme icon names sometimes appear as moz-icon://.stock/name
  nsAutoCString path;
  icon.mURI->GetPathQueryRef(path);
  if (StringBeginsWith(path, "/"_ns)) {
    path.Cut(0, 1);
  }
  // stock/foo or just foo
  if (StringBeginsWith(path, "stock/"_ns)) {
    path.Cut(0, 6);
  }
  if (!path.IsEmpty()) {
    QIcon qicon = QIcon::fromTheme(QString::fromUtf8(path.get()));
    if (!qicon.isNull()) {
      aAction->setIcon(qicon);
    }
  }
}

}  // namespace mozilla::widget
