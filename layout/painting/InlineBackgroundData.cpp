/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "InlineBackgroundData.h"

#include "nsBlockFrame.h"
#include "nsInlineFrame.h"
#include "nsLayoutUtils.h"
#include "nsRubyTextContainerFrame.h"

namespace mozilla {

static nsIFrame* GetNextContinuation(nsIFrame* aFrame) {
  nsIFrame* nextCont = aFrame->GetNextContinuation();
  if (!nextCont && aFrame->HasAnyStateBits(NS_FRAME_PART_OF_IBSPLIT)) {
    // The {ib} properties are only stored on first continuations
    aFrame = aFrame->FirstContinuation();
    nsIFrame* block = aFrame->GetProperty(nsIFrame::IBSplitSibling());
    if (block) {
      nextCont = block->GetProperty(nsIFrame::IBSplitSibling());
      NS_ASSERTION(nextCont, "How did that happen?");
    }
  }
  return nextCont;
}

static nsIFrame* GetPrevContinuation(nsIFrame* aFrame) {
  nsIFrame* prevCont = aFrame->GetPrevContinuation();
  if (!prevCont && aFrame->HasAnyStateBits(NS_FRAME_PART_OF_IBSPLIT)) {
    nsIFrame* block = aFrame->GetProperty(nsIFrame::IBSplitPrevSibling());
    if (block) {
      // The {ib} properties are only stored on first continuations
      NS_ASSERTION(!block->GetPrevContinuation(),
                   "Incorrect value for IBSplitPrevSibling");
      prevCont = block->GetProperty(nsIFrame::IBSplitPrevSibling());
      NS_ASSERTION(prevCont, "How did that happen?");
    }
  }
  return prevCont;
}

void InlineBackgroundData::SetFrame(nsIFrame* aFrame) {
  MOZ_ASSERT(aFrame, "Need a frame");

  if (aFrame == mFrame) {
    return;
  }

  nsIFrame* prevContinuation = GetPrevContinuation(aFrame);

  if (!prevContinuation || mFrame != prevContinuation) {
    // Ok, we've got the wrong frame.  We have to start from scratch.
    Reset();
    Init(aFrame);
    return;
  }

  // Get our last frame's size and add its width to our continuation
  // point before we cache the new frame.
  mContinuationPoint +=
      mVertical ? mFrame->GetSize().height : mFrame->GetSize().width;

  // If this a new line, update mLineContinuationPoint.
  if (mBidiEnabled &&
      (aFrame->GetPrevInFlow() || !AreOnSameLine(mFrame, aFrame))) {
    mLineContinuationPoint = mContinuationPoint;
  }

  mFrame = aFrame;
}

void InlineBackgroundData::Init(nsIFrame* aFrame) {
  mPIStartBorderData.Reset();
  mBidiEnabled = aFrame->PresContext()->BidiEnabled();
  if (mBidiEnabled) {
    // Find the line container frame
    mLineContainer = aFrame;
    while (mLineContainer && mLineContainer->IsLineParticipant()) {
      mLineContainer = mLineContainer->GetParent();
    }

    MOZ_ASSERT(mLineContainer, "Cannot find line containing frame.");
    MOZ_ASSERT(mLineContainer != aFrame,
               "line container frame "
               "should be an ancestor of the target frame.");
  }

  mVertical = aFrame->GetWritingMode().IsVertical();

  // Start with the previous flow frame as our continuation point
  // is the total of the widths of the previous frames.
  nsIFrame* inlineFrame = GetPrevContinuation(aFrame);
  bool changedLines = false;
  while (inlineFrame) {
    if (!mPIStartBorderData.mFrame &&
        !(mVertical ? inlineFrame->GetSkipSides().Top()
                    : inlineFrame->GetSkipSides().Left())) {
      mPIStartBorderData.mFrame = inlineFrame;
    }
    nsRect rect = inlineFrame->GetRect();
    mContinuationPoint += mVertical ? rect.height : rect.width;
    if (mBidiEnabled && (changedLines || !AreOnSameLine(aFrame, inlineFrame))) {
      mLineContinuationPoint += mVertical ? rect.height : rect.width;
      changedLines = true;
    }
    mUnbrokenMeasure += mVertical ? rect.height : rect.width;
    mBoundingBox.UnionRect(mBoundingBox, rect);
    inlineFrame = GetPrevContinuation(inlineFrame);
  }

  // Next add this frame and subsequent frames to the bounding box and
  // unbroken width.
  inlineFrame = aFrame;
  while (inlineFrame) {
    if (!mPIStartBorderData.mFrame &&
        !(mVertical ? inlineFrame->GetSkipSides().Top()
                    : inlineFrame->GetSkipSides().Left())) {
      mPIStartBorderData.mFrame = inlineFrame;
    }
    nsRect rect = inlineFrame->GetRect();
    mUnbrokenMeasure += mVertical ? rect.height : rect.width;
    mBoundingBox.UnionRect(mBoundingBox, rect);
    inlineFrame = GetNextContinuation(inlineFrame);
  }

  mFrame = aFrame;
}

bool InlineBackgroundData::AreOnSameLine(nsIFrame* aFrame1, nsIFrame* aFrame2) {
  if (nsBlockFrame* blockFrame = do_QueryFrame(mLineContainer)) {
    bool isValid1, isValid2;
    nsBlockInFlowLineIterator it1(blockFrame, aFrame1, &isValid1);
    nsBlockInFlowLineIterator it2(blockFrame, aFrame2, &isValid2);
    return isValid1 && isValid2 &&
           // Make sure aFrame1 and aFrame2 are in the same continuation of
           // blockFrame.
           it1.GetContainer() == it2.GetContainer() &&
           // And on the same line in it
           it1.GetLine().get() == it2.GetLine().get();
  }
  if (nsRubyTextContainerFrame* rtcFrame = do_QueryFrame(mLineContainer)) {
    nsBlockFrame* block = nsLayoutUtils::FindNearestBlockAncestor(rtcFrame);
    // Ruby text container can only hold one line of text, so if they
    // are in the same continuation, they are in the same line. Since
    // ruby text containers are bidi isolate, they are never split for
    // bidi reordering, which means being in different continuation
    // indicates being in different lines.
    for (nsIFrame* frame = rtcFrame->FirstContinuation(); frame;
         frame = frame->GetNextContinuation()) {
      bool isDescendant1 =
          nsLayoutUtils::IsProperAncestorFrame(frame, aFrame1, block);
      bool isDescendant2 =
          nsLayoutUtils::IsProperAncestorFrame(frame, aFrame2, block);
      if (isDescendant1 && isDescendant2) {
        return true;
      }
      if (isDescendant1 || isDescendant2) {
        return false;
      }
    }
    MOZ_ASSERT_UNREACHABLE("None of the frames is a descendant of this rtc?");
  }
  MOZ_ASSERT_UNREACHABLE("Do we have any other type of line container?");
  return false;
}

nsRect InlineBackgroundData::GetContinuousRect(nsIFrame* aFrame) {
  MOZ_ASSERT(static_cast<nsInlineFrame*>(do_QueryFrame(aFrame)));

  SetFrame(aFrame);

  nscoord pos;  // an x coordinate if writing-mode is horizontal;
                // y coordinate if vertical
  if (mBidiEnabled) {
    pos = mLineContinuationPoint;

    // Scan continuations on the same line as aFrame and accumulate the widths
    // of frames that are to the left (if this is an LTR block) or right
    // (if it's RTL) of the current one.
    bool isRtlBlock =
        (mLineContainer->StyleVisibility()->mDirection == StyleDirection::Rtl);
    nscoord curOffset = mVertical ? aFrame->GetOffsetTo(mLineContainer).y
                                  : aFrame->GetOffsetTo(mLineContainer).x;

    // If the continuation is fluid we know inlineFrame is not on the same
    // line. If it's not fluid, we need to test further to be sure.
    nsIFrame* inlineFrame = aFrame->GetPrevContinuation();
    while (inlineFrame && !inlineFrame->GetNextInFlow() &&
           AreOnSameLine(aFrame, inlineFrame)) {
      nscoord frameOffset = mVertical
                                ? inlineFrame->GetOffsetTo(mLineContainer).y
                                : inlineFrame->GetOffsetTo(mLineContainer).x;
      if (isRtlBlock == (frameOffset >= curOffset)) {
        pos += mVertical ? inlineFrame->GetSize().height
                         : inlineFrame->GetSize().width;
      }
      inlineFrame = inlineFrame->GetPrevContinuation();
    }

    inlineFrame = aFrame->GetNextContinuation();
    while (inlineFrame && !inlineFrame->GetPrevInFlow() &&
           AreOnSameLine(aFrame, inlineFrame)) {
      nscoord frameOffset = mVertical
                                ? inlineFrame->GetOffsetTo(mLineContainer).y
                                : inlineFrame->GetOffsetTo(mLineContainer).x;
      if (isRtlBlock == (frameOffset >= curOffset)) {
        pos += mVertical ? inlineFrame->GetSize().height
                         : inlineFrame->GetSize().width;
      }
      inlineFrame = inlineFrame->GetNextContinuation();
    }
    if (isRtlBlock) {
      // aFrame itself is also to the right of its left edge, so add its
      // width.
      pos += mVertical ? aFrame->GetSize().height : aFrame->GetSize().width;
      // pos is now the distance from the left [top] edge of aFrame to the
      // right [bottom] edge of the unbroken content. Change it to indicate
      // the distance from the left [top] edge of the unbroken content to the
      // left [top] edge of aFrame.
      pos = mUnbrokenMeasure - pos;
    }
  } else {
    pos = mContinuationPoint;
  }

  // Assume background-origin: border and return a rect with offsets
  // relative to (0,0).  If we have a different background-origin,
  // then our rect should be deflated appropriately by our caller.
  return mVertical
             ? nsRect(0, -pos, mFrame->GetSize().width, mUnbrokenMeasure)
             : nsRect(-pos, 0, mUnbrokenMeasure, mFrame->GetSize().height);
}

/**
 * Return a continuous rect for (an inline) aFrame relative to the
 * continuation that should draw the left[top]-border.  This is used when
 * painting borders and clipping backgrounds.  This may NOT be the same
 * continuous rect as for drawing backgrounds; the continuation with the
 * left[top]-border might be somewhere in the middle of that rect (e.g. BIDI),
 * in those cases we need the reverse background order starting at the
 * left[top]-border continuation.
 */
nsRect InlineBackgroundData::GetBorderContinuousRect(nsIFrame* aFrame,
                                                     nsRect aBorderArea) {
  // Calling GetContinuousRect(aFrame) here may lead to Reset/Init which
  // resets our mPIStartBorderData so we save it ...
  PhysicalInlineStartBorderData saved(mPIStartBorderData);
  nsRect joinedBorderArea = GetContinuousRect(aFrame);
  if (!saved.mIsValid || saved.mFrame != mPIStartBorderData.mFrame) {
    if (aFrame == mPIStartBorderData.mFrame) {
      if (mVertical) {
        mPIStartBorderData.SetCoord(joinedBorderArea.y);
      } else {
        mPIStartBorderData.SetCoord(joinedBorderArea.x);
      }
    } else if (mPIStartBorderData.mFrame) {
      // Copy data to a temporary object so that computing the
      // continous rect here doesn't clobber our normal state.
      InlineBackgroundData temp = *this;
      if (mVertical) {
        mPIStartBorderData.SetCoord(
            temp.GetContinuousRect(mPIStartBorderData.mFrame).y);
      } else {
        mPIStartBorderData.SetCoord(
            temp.GetContinuousRect(mPIStartBorderData.mFrame).x);
      }
    }
  } else {
    // ... and restore it when possible.
    mPIStartBorderData.SetCoord(saved.mCoord);
  }
  if (mVertical) {
    if (joinedBorderArea.y > mPIStartBorderData.mCoord) {
      joinedBorderArea.y =
          -(mUnbrokenMeasure + joinedBorderArea.y - aBorderArea.height);
    } else {
      joinedBorderArea.y -= mPIStartBorderData.mCoord;
    }
  } else {
    if (joinedBorderArea.x > mPIStartBorderData.mCoord) {
      joinedBorderArea.x =
          -(mUnbrokenMeasure + joinedBorderArea.x - aBorderArea.width);
    } else {
      joinedBorderArea.x -= mPIStartBorderData.mCoord;
    }
  }
  return joinedBorderArea;
}

nsRect InlineBackgroundData::GetBoundingRect(nsIFrame* aFrame) {
  SetFrame(aFrame);

  // Move the offsets relative to (0,0) which puts the bounding box into
  // our coordinate system rather than our parent's.  We do this by
  // moving it the back distance from us to the bounding box.
  // This also assumes background-origin: border, so our caller will
  // need to deflate us if needed.
  nsRect boundingBox(mBoundingBox);
  nsPoint point = mFrame->GetPosition();
  boundingBox.MoveBy(-point.x, -point.y);

  return boundingBox;
}

}  // namespace mozilla
