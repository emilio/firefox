/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_SCALEDFONTBASE_H_
#define MOZILLA_GFX_SCALEDFONTBASE_H_

#include "2D.h"
#include "skia/include/core/SkFont.h"
#include "skia/include/core/SkPath.h"
#include "skia/include/core/SkTypeface.h"

namespace mozilla {
namespace gfx {

class ScaledFontBase : public ScaledFont {
 public:
  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(ScaledFontBase, override)

  ScaledFontBase(const RefPtr<UnscaledFont>& aUnscaledFont, Float aSize);
  virtual ~ScaledFontBase();

  virtual already_AddRefed<Path> GetPathForGlyphs(
      const GlyphBuffer& aBuffer, const DrawTarget* aTarget) override;

  virtual void CopyGlyphsToBuilder(const GlyphBuffer& aBuffer,
                                   PathBuilder* aBuilder,
                                   const Matrix* aTransformHint) override;

  virtual Float GetSize() const override { return mSize; }

  SkTypeface* GetSkTypeface();
  virtual void SetupSkFontDrawOptions(SkFont& aFont) {}

 protected:
  friend class DrawTargetSkia;
  Atomic<SkTypeface*> mTypeface;
  virtual SkTypeface* CreateSkTypeface() { return nullptr; }
  SkPath GetSkiaPathForGlyphs(const GlyphBuffer& aBuffer);
  Float mSize;
};

}  // namespace gfx
}  // namespace mozilla

#endif /* MOZILLA_GFX_SCALEDFONTBASE_H_ */
