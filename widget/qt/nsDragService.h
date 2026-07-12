/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_qt_nsDragService_h
#define widget_qt_nsDragService_h

#include "nsBaseDragService.h"
#include "nsCOMArray.h"
#include "nsCOMPtr.h"
#include "nsIArray.h"
#include "nsIFile.h"
#include "nsString.h"
#include "mozilla/UniquePtr.h"

class QMimeData;
class QPixmap;

/**
 * Qt drag session: outbound QDrag + inbound QMimeData for drops into Gecko.
 */
class nsDragSession final : public nsBaseDragSession {
 public:
  NS_IMETHOD GetData(nsITransferable* aTransferable,
                     uint32_t aItemIndex) override;
  NS_IMETHOD GetNumDropItems(uint32_t* aNumItems) override;
  NS_IMETHOD IsDataFlavorSupported(const char* aDataFlavor,
                                   bool* _retval) override;

  // Copy platform mime data for an external (or internal) drop target session.
  void SetDropMimeData(const QMimeData* aMime);
  const QMimeData* GetDropMimeData() const { return mDropMime.get(); }

 protected:
  ~nsDragSession() override;
  MOZ_CAN_RUN_SCRIPT nsresult
  InvokeDragSessionImpl(nsIWidget* aWidget, nsIArray* anArrayTransferables,
                        const mozilla::Maybe<mozilla::CSSIntRegion>& aRegion,
                        uint32_t aActionType) override;

 private:
  // Build drag feedback pixmap from mSourceNode / mImage (DrawDrag).
  // Hotspot returned in pixmap pixel coordinates.
  bool ConstructDragPixmap(const mozilla::Maybe<mozilla::CSSIntRegion>& aRegion,
                           QPixmap* aOutPixmap, int* aHotspotX, int* aHotspotY);

  // Download kFilePromiseMime items into temp files; append local paths.
  void MaterializeFilePromises(nsIArray* aTransferables,
                               nsTArray<nsString>& aLocalPathsOut);

  void CleanupTempFiles();

  // Source-side transferables (drag from Gecko).
  nsCOMPtr<nsIArray> mSourceTransferables;
  // Target-side copy of QMimeData for the current drop session.
  mozilla::UniquePtr<QMimeData> mDropMime;
  // Temp dirs created for file-promise materialization.
  nsCOMArray<nsIFile> mTemporaryDirs;
};

class nsDragService final : public nsBaseDragService {
 public:
  static already_AddRefed<nsDragService> GetInstance();

 protected:
  already_AddRefed<nsIDragSession> CreateDragSession() override;
};

// Shared transferable ↔ QMimeData helpers (also usable by clipboard later).
namespace mozilla::widget::qt_dnd {
// Build QMimeData from a Gecko transferable array. Caller owns the result.
// aExtraLocalPaths: optional absolute file paths already materialized.
QMimeData* MimeDataFromTransferables(
    nsIArray* aTransferables,
    const nsTArray<nsString>& aExtraLocalPaths = nsTArray<nsString>());
// Put one flavor from QMimeData into aTransferable.
bool TransferableFromMimeData(const QMimeData* aMime, const char* aFlavor,
                              nsITransferable* aTransferable,
                              uint32_t aItemIndex);
bool MimeSupportsFlavor(const QMimeData* aMime, const char* aFlavor);
uint32_t MimeNumItems(const QMimeData* aMime);
}  // namespace mozilla::widget::qt_dnd

#endif
