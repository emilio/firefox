/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_windows_InProcessWinCompositorWidget_h
#define widget_windows_InProcessWinCompositorWidget_h

#include "WinCompositorWidget.h"

class nsWindow;

namespace mozilla::widget {

// This is the Windows-specific implementation of CompositorWidget. For
// the most part it only requires an HWND, however it maintains extra state
// for transparent windows, as well as for synchronizing WM_SETTEXT messages
// with the compositor.
class InProcessWinCompositorWidget final
    : public WinCompositorWidget,
      public PlatformCompositorWidgetDelegate {
 public:
  InProcessWinCompositorWidget(const WinCompositorWidgetInitData& aInitData,
                               const layers::CompositorOptions& aOptions,
                               nsWindow* aWindow);
  ~InProcessWinCompositorWidget() override;

  bool PreRender(WidgetRenderingContext*) override;
  void PostRender(WidgetRenderingContext*) override;
  already_AddRefed<gfx::DrawTarget> StartRemoteDrawing() override;
  void EndRemoteDrawing() override;
  LayoutDeviceIntSize GetClientSize() override;
  bool InitCompositor(layers::Compositor* aCompositor) override;
  CompositorWidgetDelegate* AsDelegate() override { return this; }
  bool IsHidden() const override;

  // PlatformCompositorWidgetDelegate Overrides

  void EnterPresentLock() override;
  void LeavePresentLock() override;
  void OnDestroyWindow() override;
  bool OnWindowResize(const LayoutDeviceIntSize& aSize) override;
  void UpdateTransparency(TransparencyMode aMode) override;
  void NotifyVisibilityUpdated(bool aIsFullyOccluded) override;

  bool GetWindowIsFullyOccluded() const override;

  void ObserveVsync(VsyncObserver* aObserver) override;
  nsIWidget* RealWidget() override;

  void UpdateCompositorWnd(const HWND aCompositorWnd,
                           const HWND aParentWnd) override;
  void SetRootLayerTreeID(const layers::LayersId& aRootLayerTreeId) override {}

 private:
  bool EnsureBackBuffer(const gfx::IntSize& aSize);
  void ReleaseBackBuffer();

  nsWindow* mWindow;

  HWND mWnd;

  gfx::CriticalSection mPresentLock;

  // Visibility handling.
  mozilla::Atomic<bool, MemoryOrdering::Relaxed> mIsFullyOccluded;

  // Memory DC with a DIB section selected, which StartRemoteDrawing() returns
  // a draw target for, and that EndRemoteDrawing() blits to the window.
  HDC mBackBufferDC = nullptr;
  HBITMAP mBackBufferBitmap = nullptr;
  HGDIOBJ mBackBufferSavedObject = nullptr;
  uint8_t* mBackBufferData = nullptr;
  gfx::IntSize mBackBufferSize;
  bool mIsDrawing = false;
};

}  // namespace mozilla::widget

#endif  // widget_windows_InProcessWinCompositorWidget_h
