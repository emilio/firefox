/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PrintTargetWindows.h"

#include "cairo-win32.h"
#include "gdi/SkGDIDocument.h"
#include "mozilla/StaticPrefs_browser.h"
#include "mozilla/StaticPrefs_print.h"
#include "mozilla/WidgetUtils.h"
#include "mozilla/gfx/HelpersCairo.h"
#include "nsCoord.h"
#include "nsIContentAnalysis.h"
#include "nsIWidget.h"
#include "nsIWindowMediator.h"
#include "nsPIDOMWindow.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"

namespace mozilla {
namespace gfx {

PrintTargetWindows::PrintTargetWindows(cairo_surface_t* aCairoSurface,
                                       const IntSize& aSize, HDC aDC)
    : PrintTarget(aCairoSurface, aSize), mDC(aDC) {
  // TODO: At least add basic memory reporting.
  // 4 * mSize.width * mSize.height + sizeof(PrintTargetWindows) ?
}

PrintTargetWindows::PrintTargetWindows(sk_sp<SkDocument> aDocument,
                                       const IntSize& aSize, HDC aDC)
    : PrintTarget(/* not using cairo_surface_t */ nullptr, aSize),
      mDC(aDC),
      mDocument(std::move(aDocument)) {}

PrintTargetWindows::~PrintTargetWindows() = default;

/* static */
already_AddRefed<PrintTargetWindows> PrintTargetWindows::CreateOrNull(HDC aDC) {
  // Figure out the paper size, the actual surface size will be the printable
  // area which is likely smaller, but the size here is later used to create the
  // draw target where the full page size is needed.
  // Note: we only scale the printing using the LOGPIXELSY,
  // so we use that when calculating the surface width as well as the height.
  int32_t heightDPI = ::GetDeviceCaps(aDC, LOGPIXELSY);
  float width =
      (::GetDeviceCaps(aDC, PHYSICALWIDTH) * POINTS_PER_INCH_FLOAT) / heightDPI;
  float height =
      (::GetDeviceCaps(aDC, PHYSICALHEIGHT) * POINTS_PER_INCH_FLOAT) /
      heightDPI;
  IntSize size = IntSize::Truncate(width, height);

  if (!Factory::CheckSurfaceSize(size)) {
    return nullptr;
  }

  if (StaticPrefs::print_windows_skia_gdi_enabled()) {
    SkGDI::Options options;
    options.fRasterScale =
        float(StaticPrefs::print_windows_skia_gdi_raster_dpi()) /
        ::GetDeviceCaps(aDC, LOGPIXELSX);
    sk_sp<SkDocument> doc = SkGDI::MakeDocument(aDC, options);
    if (!doc) {
      return nullptr;
    }
    return do_AddRef(new PrintTargetWindows(std::move(doc), size, aDC));
  }

  cairo_surface_t* surface = cairo_win32_printing_surface_create(aDC);

  if (cairo_surface_status(surface)) {
    return nullptr;
  }

  // The new object takes ownership of our surface reference.
  RefPtr<PrintTargetWindows> target =
      new PrintTargetWindows(surface, size, aDC);

  return target.forget();
}

LazyLogModule gPrintingLog("printing");

nsresult PrintTargetWindows::BeginPrinting(const nsAString& aTitle,
                                           const nsAString& aPrintToFileName,
                                           uint64_t aInnerWindowId,
                                           int32_t aStartPage,
                                           int32_t aEndPage) {
  const uint32_t DOC_TITLE_LENGTH = MAX_PATH - 1;

  DOCINFOW docinfo;

  nsString titleStr(aTitle);
  if (titleStr.Length() > DOC_TITLE_LENGTH) {
    titleStr.SetLength(DOC_TITLE_LENGTH - 3);
    titleStr.AppendLiteral("...");
  }

  nsString docName(aPrintToFileName);
  docinfo.cbSize = sizeof(docinfo);
  docinfo.lpszDocName =
      titleStr.Length() > 0 ? titleStr.get() : L"Mozilla Document";
  docinfo.lpszOutput = docName.Length() > 0 ? docName.get() : nullptr;
  docinfo.lpszDatatype = nullptr;
  docinfo.fwType = 0;

  // If we just did content analysis on this print request, it may have popped
  // up a dialog, and this can prevent StartDocW() from working properly if the
  // printer wants to pop up a dialog window (to get a file name to save to, for
  // example). Setting the foreground window to any browser window seems to work
  // around this. See bug 1980225.
  if (nsIContentAnalysis::MightBeActive() &&
      StaticPrefs::browser_contentanalysis_print_set_foreground_window()) {
    nsCOMPtr<nsIWindowMediator> winMediator =
        do_GetService(NS_WINDOWMEDIATOR_CONTRACTID);
    if (winMediator) {
      nsCOMPtr<mozIDOMWindowProxy> domWindow;
      nsresult rv =
          winMediator->GetMostRecentBrowserWindow(getter_AddRefs(domWindow));
      if (NS_SUCCEEDED(rv) && domWindow) {
        nsPIDOMWindowOuter* win = nsPIDOMWindowOuter::From(domWindow);
        if (win) {
          nsCOMPtr<nsIWidget> widget =
              widget::WidgetUtils::DOMWindowToWidget(win);
          if (widget) {
            HWND hwnd =
                static_cast<HWND>(widget->GetNativeData(NS_NATIVE_WINDOW));
            BOOL foregroundReturn = ::SetForegroundWindow(hwnd);
            MOZ_LOG(gPrintingLog, mozilla::LogLevel::Debug,
                    ("Called SetForegroundWindow(), which returned %d",
                     foregroundReturn));
          }
        }
      }
    }
  }
  // If the user selected Microsoft Print to PDF or XPS Document Printer, then
  // the following StartDoc call will put up a dialog window to prompt the
  // user to provide the name and location of the file to be saved.  A zero or
  // negative return value indicates failure.  In that case we want to check
  // whether that is because the user hit Cancel, since we want to treat that
  // specially to avoid notifying the user that the print "failed" in that
  // case.
  // XXX We should perhaps introduce a new NS_ERROR_USER_CANCELLED errer.
  int result = ::StartDocW(mDC, &docinfo);
  if (result <= 0) {
    DWORD lastError = ::GetLastError();
    if (lastError == ERROR_CANCELLED) {
      return NS_ERROR_ABORT;
    }
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

nsresult PrintTargetWindows::EndPrinting() {
  mPageCanvas = nullptr;
  mPageDT = nullptr;
  if (mDocument) {
    mDocument->close();
  }
  int result = ::EndDoc(mDC);
  return (result <= 0) ? NS_ERROR_FAILURE : NS_OK;
}

nsresult PrintTargetWindows::AbortPrinting() {
  PrintTarget::AbortPrinting();
  mPageCanvas = nullptr;
  mPageDT = nullptr;
  if (mDocument) {
    mDocument->abort();
  }
  int result = ::AbortDoc(mDC);
  return (result <= 0) ? NS_ERROR_FAILURE : NS_OK;
}

nsresult PrintTargetWindows::BeginPage(const IntSize& aSizeInPoints) {
  MOZ_ALWAYS_SUCCEEDS(PrintTarget::BeginPage(aSizeInPoints));
  int result = ::StartPage(mDC);
  if (result <= 0) {
    return NS_ERROR_FAILURE;
  }
  if (mDocument) {
    // The page canvas is in device units of the printable area, which is what
    // the content is drawn in (see nsIDeviceContextSpec::GetPrintingScale).
    mPageCanvas = mDocument->beginPage(::GetDeviceCaps(mDC, HORZRES),
                                       ::GetDeviceCaps(mDC, VERTRES));
    if (!mPageCanvas) {
      return NS_ERROR_FAILURE;
    }
  }
  return NS_OK;
}

nsresult PrintTargetWindows::EndPage() {
  bool failure = false;
  if (mDocument) {
    mPageDT = nullptr;
    mPageCanvas = nullptr;
    mDocument->endPage();
  } else {
    cairo_surface_show_page(mCairoSurface);
    failure = cairo_surface_status(mCairoSurface);
  }
  MOZ_ALWAYS_SUCCEEDS(PrintTarget::EndPage());
  int result = ::EndPage(mDC);
  return (result <= 0 || failure) ? NS_ERROR_FAILURE : NS_OK;
}

already_AddRefed<DrawTarget> PrintTargetWindows::MakeDrawTarget(
    const IntSize& aSize, DrawEventRecorder* aRecorder) {
  if (!mDocument || aRecorder) {
    return PrintTarget::MakeDrawTarget(aSize, aRecorder);
  }
  MOZ_ASSERT(mHasActivePage, "We can't guarantee a valid DrawTarget");
  if (!mPageCanvas) {
    return nullptr;
  }
  mPageDT = Factory::CreateDrawTargetWithSkCanvas(mPageCanvas);
  return do_AddRef(mPageDT);
}

already_AddRefed<DrawTarget> PrintTargetWindows::GetReferenceDrawTarget() {
  if (!mDocument) {
    return PrintTarget::GetReferenceDrawTarget();
  }
  if (!mRefDT) {
    mRefDT = Factory::CreateDrawTarget(BackendType::SKIA, IntSize(1, 1),
                                       SurfaceFormat::B8G8R8A8);
  }
  return do_AddRef(mRefDT);
}

}  // namespace gfx
}  // namespace mozilla
