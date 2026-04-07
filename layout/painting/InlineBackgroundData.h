/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_InlineBackgroundData_h
#define mozilla_InlineBackgroundData_h

#include "nsRect.h"

class nsIFrame;

namespace mozilla {

// To avoid storing this data on nsInlineFrame (bloat) and to avoid
// recalculating this for each frame in a continuation (perf), hold
// a cache of various coordinate information that we need in order
// to paint inline backgrounds.
struct InlineBackgroundData {
  InlineBackgroundData() = default;
  ~InlineBackgroundData() = default;

  /**
   * Return a continuous rect for (an inline) aFrame relative to the
   * continuation that draws the left-most part of the background.
   * This is used when painting backgrounds.
   */
  nsRect GetContinuousRect(nsIFrame* aFrame);

  /**
   * Return a continuous rect for (an inline) aFrame relative to the
   * continuation that should draw the left[top]-border.  This is used when
   * painting borders and clipping backgrounds.  This may NOT be the same
   * continuous rect as for drawing backgrounds; the continuation with the
   * left[top]-border might be somewhere in the middle of that rect (e.g. BIDI),
   * in those cases we need the reverse background order starting at the
   * left[top]-border continuation.
   */
  nsRect GetBorderContinuousRect(nsIFrame* aFrame, nsRect aBorderArea);
  nsRect GetBoundingRect(nsIFrame* aFrame);

 protected:
  // This is a coordinate on the inline axis, but is not a true logical inline-
  // coord because it is always measured from left to right (if horizontal) or
  // from top to bottom (if vertical), ignoring any bidi RTL directionality.
  // We'll call this "physical inline start", or PIStart for short.
  struct PhysicalInlineStartBorderData {
    // the continuation that may have a left-border
    nsIFrame* mFrame = nullptr;
    nscoord mCoord = 0;     // cached GetContinuousRect(mFrame).x or .y
    bool mIsValid = false;  // true if mCoord is valid
    void Reset() { *this = {}; }
    void SetCoord(nscoord aCoord) {
      mCoord = aCoord;
      mIsValid = true;
    }
  };

  nsIFrame* mFrame = nullptr;
  nsIFrame* mLineContainer = nullptr;
  nsRect mBoundingBox;
  nscoord mContinuationPoint = 0;
  nscoord mUnbrokenMeasure = 0;
  nscoord mLineContinuationPoint = 0;
  PhysicalInlineStartBorderData mPIStartBorderData;
  bool mBidiEnabled = false;
  bool mVertical = false;

  void Reset() { *this = {}; }
  void SetFrame(nsIFrame* aFrame);
  void Init(nsIFrame* aFrame);
  bool AreOnSameLine(nsIFrame* aFrame1, nsIFrame* aFrame2);
};

}  // namespace mozilla

#endif
