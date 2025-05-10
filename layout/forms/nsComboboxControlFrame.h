/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsComboboxControlFrame_h___
#define nsComboboxControlFrame_h___

#include "mozilla/Attributes.h"
#include "nsIAnonymousContentCreator.h"
#include "nsISelectControlFrame.h"
#include "nsIRollupListener.h"
#include "nsThreadUtils.h"
#include "nsGfxButtonControlFrame.h"

namespace mozilla {
class PresShell;
class HTMLSelectEventListener;
class ComboboxLabelFrame;
namespace dom {
class HTMLSelectElement;
}
}  // namespace mozilla

class nsComboboxControlFrame final : public nsGfxButtonControlFrame,
                                     public nsISelectControlFrame {
  using Element = mozilla::dom::Element;

 public:
  friend class mozilla::ComboboxLabelFrame;
  explicit nsComboboxControlFrame(ComputedStyle* aStyle,
                                  nsPresContext* aPresContext);
  ~nsComboboxControlFrame();

  NS_DECL_QUERYFRAME
  NS_DECL_FRAMEARENA_HELPERS(nsComboboxControlFrame)

  // nsIAnonymousContentCreator
  nsresult CreateAnonymousContent(nsTArray<ContentInfo>& aElements) final;
  void AppendAnonymousContentTo(nsTArray<nsIContent*>& aElements,
                                uint32_t aFilter) final;

#ifdef ACCESSIBILITY
  mozilla::a11y::AccType AccessibleType() final;
#endif

  nscoord IntrinsicISize(const mozilla::IntrinsicSizeInput& aInput,
                         mozilla::IntrinsicISizeType aType) final;
  nsresult GetLabel(nsString&) override;

  void Reflow(nsPresContext* aCX, ReflowOutput& aDesiredSize,
              const ReflowInput& aReflowInput, nsReflowStatus& aStatus) final;

  MOZ_CAN_RUN_SCRIPT_BOUNDARY
  nsresult HandleEvent(nsPresContext* aPresContext,
                       mozilla::WidgetGUIEvent* aEvent,
                       nsEventStatus* aEventStatus) final;

  void Init(nsIContent* aContent, nsContainerFrame* aParent,
            nsIFrame* aPrevInFlow) final;
  void Destroy(DestroyContext&) final;

#ifdef DEBUG_FRAME_DUMP
  nsresult GetFrameName(nsAString& aResult) const final {
    return MakeFrameName(u"ComboboxControl"_ns, aResult);
  }
#endif

  /**
   * @note This method might destroy |this|.
   */
  void FireValueChangeEvent();
  nsresult RedisplaySelectedText();

  bool IsDroppedDown() const;

  // nsISelectControlFrame
  NS_IMETHOD AddOption(int32_t index) final;
  NS_IMETHOD RemoveOption(int32_t index) final;
  NS_IMETHOD DoneAddingChildren(bool aIsDone) final;
  NS_IMETHOD OnOptionSelected(int32_t aIndex, bool aSelected) final;
  NS_IMETHOD_(void)
  OnSetSelectedIndex(int32_t aOldIndex, int32_t aNewIndex) final;

  int32_t CharCountOfLargestOptionForInflation() const;

 protected:
  friend class RedisplayTextEvent;
  friend class nsAsyncResize;
  friend class nsResizeDropdownAtFinalPosition;

  // Return true if we should render a dropdown button.
  bool HasDropDownButton() const;
  nscoord DropDownButtonISize();

  nscoord GetLongestOptionISize(gfxContext*) const;

  class RedisplayTextEvent : public mozilla::Runnable {
   public:
    NS_DECL_NSIRUNNABLE
    explicit RedisplayTextEvent(nsComboboxControlFrame* c)
        : mozilla::Runnable("nsComboboxControlFrame::RedisplayTextEvent"),
          mControlFrame(c) {}
    void Revoke() { mControlFrame = nullptr; }

   private:
    nsComboboxControlFrame* mControlFrame;
  };

  nsresult RedisplayText();
  void HandleRedisplayTextEvent();

  mozilla::dom::HTMLSelectElement& Select() const;
  void GetOptionText(uint32_t aIndex, nsAString& aText) const;

  RefPtr<Element> mButtonContent;  // Anonymous content for the button
  nsRevocableEventPtr<RedisplayTextEvent> mRedisplayTextEvent;

  int32_t mDisplayedIndex = -1;
  RefPtr<mozilla::HTMLSelectEventListener> mEventListener;
};

#endif
