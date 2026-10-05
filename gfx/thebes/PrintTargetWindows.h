/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_PRINTTARGETWINDOWS_H
#define MOZILLA_GFX_PRINTTARGETWINDOWS_H

#include "PrintTarget.h"
#include "skia/include/core/SkRefCnt.h"

/* include windows.h for the HDC definitions that we need. */
#include <windows.h>

class SkCanvas;
class SkDocument;

namespace mozilla {
namespace gfx {

/**
 * Windows printing target.
 *
 * Draws either through Skia's GDI document backend (SkGDIDocument) or, if
 * disabled, through cairo's win32 printing surface.
 */
class PrintTargetWindows final : public PrintTarget {
 public:
  static already_AddRefed<PrintTargetWindows> CreateOrNull(HDC aDC);

  nsresult BeginPrinting(const nsAString& aTitle,
                         const nsAString& aPrintToFileName,
                         uint64_t aInnerWindowId, int32_t aStartPage,
                         int32_t aEndPage) override;
  nsresult EndPrinting() override;
  nsresult AbortPrinting() override;
  nsresult BeginPage(const IntSize& aSizeInPoints) override;
  nsresult EndPage() override;

  already_AddRefed<DrawTarget> MakeDrawTarget(
      const IntSize& aSize, DrawEventRecorder* aRecorder = nullptr) final;
  already_AddRefed<DrawTarget> GetReferenceDrawTarget() final;

 private:
  PrintTargetWindows(cairo_surface_t* aCairoSurface, const IntSize& aSize,
                     HDC aDC);
  PrintTargetWindows(sk_sp<SkDocument> aDocument, const IntSize& aSize,
                     HDC aDC);
  ~PrintTargetWindows() override;

  HDC mDC;

  // Only used for Skia printing. The document draws into mDC, and owns
  // mPageCanvas, which is only valid between BeginPage() and EndPage().
  sk_sp<SkDocument> mDocument;
  SkCanvas* mPageCanvas = nullptr;
  RefPtr<DrawTarget> mPageDT;
};

}  // namespace gfx
}  // namespace mozilla

#endif /* MOZILLA_GFX_PRINTTARGETWINDOWS_H */
