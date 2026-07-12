/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DBusMenu.h"
#include "prlink.h"
#include "nsThreadUtils.h"

namespace mozilla::widget {

#define FUNC(name, type, params) \
  DBusMenuFunctions::_##name##_fn DBusMenuFunctions::s_##name;
DBUSMENU_GLIB_FUNCTIONS
#undef FUNC

static PRLibrary* gDbusmenuGlib = nullptr;

using DBusMenuFunc = void (*)();
struct DBusMenuDynamicFunction {
  const char* functionName;
  DBusMenuFunc* function;
};

static bool sInitialized;
static bool sLibPresent;

/* static */
bool DBusMenuFunctions::Init() {
  MOZ_ASSERT(NS_IsMainThread());
  if (sInitialized) {
    return sLibPresent;
  }
  sInitialized = true;

#define FUNC(name, type, params) \
  {#name, (DBusMenuFunc*)&DBusMenuFunctions::s_##name},
  static const DBusMenuDynamicFunction kDbusmenuGlibSymbols[] = {
      DBUSMENU_GLIB_FUNCTIONS};
#undef FUNC

  gDbusmenuGlib = PR_LoadLibrary("libdbusmenu-glib.so.4");
  if (!gDbusmenuGlib) {
    return false;
  }
  for (uint32_t i = 0; i < std::size(kDbusmenuGlibSymbols); ++i) {
    *kDbusmenuGlibSymbols[i].function = PR_FindFunctionSymbol(
        gDbusmenuGlib, kDbusmenuGlibSymbols[i].functionName);
    if (!*kDbusmenuGlibSymbols[i].function) {
      return false;
    }
  }

  sLibPresent = true;
  return true;
}

}  // namespace mozilla::widget
