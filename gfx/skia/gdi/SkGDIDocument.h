/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SkGDIDocument_DEFINED
#define SkGDIDocument_DEFINED

#include "include/core/SkDocument.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"

#include <windows.h>

namespace SkGDI {

struct Options {
    // Resolution of the bitmaps used for content that GDI can't express (blending, non-trivial
    // layers, filters...), relative to the device resolution. Should be <= 1.
    SkScalar fRasterScale = 1.0f;
};

/**
 * Creates a document that replays each page into |dc| as GDI calls, rasterizing only the regions
 * of the page that GDI can't express, in the spirit of SkPDF and cairo's win32 printing surface.
 *
 * The caller remains responsible for StartDoc/StartPage/EndPage/EndDoc: the page contents are
 * emitted to |dc| when SkDocument::endPage() is called. Canvas units are the device units of
 * |dc|.
 */
SK_API sk_sp<SkDocument> MakeDocument(HDC dc, const Options& options = Options());

}  // namespace SkGDI

#endif
