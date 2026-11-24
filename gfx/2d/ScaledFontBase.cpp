/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScaledFontBase.h"

#include <vector>

#include "PathSkia.h"
#include "skia/include/core/SkFont.h"

namespace mozilla {
namespace gfx {

Atomic<uint32_t> UnscaledFont::sDeletionCounter(0);

UnscaledFont::~UnscaledFont() { sDeletionCounter++; }

Atomic<uint32_t> ScaledFont::sDeletionCounter(0);

ScaledFont::~ScaledFont() { sDeletionCounter++; }

ScaledFontBase::~ScaledFontBase() { SkSafeUnref<SkTypeface>(mTypeface); }

ScaledFontBase::ScaledFontBase(const RefPtr<UnscaledFont>& aUnscaledFont,
                               Float aSize)
    : ScaledFont(aUnscaledFont), mTypeface(nullptr), mSize(aSize) {}

SkTypeface* ScaledFontBase::GetSkTypeface() {
  if (!mTypeface) {
    SkTypeface* typeface = CreateSkTypeface();
    if (!mTypeface.compareExchange(nullptr, typeface)) {
      SkSafeUnref(typeface);
    }
  }
  return mTypeface;
}

SkPath ScaledFontBase::GetSkiaPathForGlyphs(const GlyphBuffer& aBuffer) {
  SkTypeface* typeFace = GetSkTypeface();
  MOZ_ASSERT(typeFace);

  SkFont font(sk_ref_sp(typeFace), SkFloatToScalar(mSize));

  std::vector<uint16_t> indices;
  indices.resize(aBuffer.mNumGlyphs);
  for (unsigned int i = 0; i < aBuffer.mNumGlyphs; i++) {
    indices[i] = aBuffer.mGlyphs[i].mIndex;
  }

  struct Context {
    const Glyph* mGlyph;
    SkPathBuilder mPathBuilder;
  } ctx = {aBuffer.mGlyphs};

  font.getPaths(
      {indices.data(), indices.size()},
      [](const SkPath* glyphPath, const SkMatrix& scaleMatrix, void* ctxPtr) {
        Context& ctx = *reinterpret_cast<Context*>(ctxPtr);
        if (glyphPath) {
          SkMatrix transMatrix(scaleMatrix);
          transMatrix.postTranslate(SkFloatToScalar(ctx.mGlyph->mPosition.x),
                                    SkFloatToScalar(ctx.mGlyph->mPosition.y));
          ctx.mPathBuilder.addPath(*glyphPath, transMatrix);
        }
        ++ctx.mGlyph;
      },
      &ctx);

  return ctx.mPathBuilder.detach();
}

already_AddRefed<Path> ScaledFontBase::GetPathForGlyphs(
    const GlyphBuffer& aBuffer, const DrawTarget* aTarget) {
  switch (aTarget->GetBackendType()) {
    case BackendType::SKIA: {
      SkPath path = GetSkiaPathForGlyphs(aBuffer);
      return MakeAndAddRef<PathSkia>(path, FillRule::FILL_WINDING);
    }
    default: {
      RefPtr<PathBuilder> builder = aTarget->CreatePathBuilder();
      SkPath skPath = GetSkiaPathForGlyphs(aBuffer);
      RefPtr<Path> path =
          MakeAndAddRef<PathSkia>(skPath, FillRule::FILL_WINDING);
      path->StreamToSink(builder);
      return builder->Finish();
    }
  }
}

void ScaledFontBase::CopyGlyphsToBuilder(const GlyphBuffer& aBuffer,
                                         PathBuilder* aBuilder,
                                         const Matrix* aTransformHint) {
  switch (aBuilder->GetBackendType()) {
    case BackendType::SKIA: {
      PathBuilderSkia* builder = static_cast<PathBuilderSkia*>(aBuilder);
      builder->AppendPath(GetSkiaPathForGlyphs(aBuffer));
      return;
    }
    case BackendType::RECORDING: {
      SkPath skPath = GetSkiaPathForGlyphs(aBuffer);
      RefPtr<Path> path =
          MakeAndAddRef<PathSkia>(skPath, FillRule::FILL_WINDING);
      path->StreamToSink(aBuilder);
      return;
    }
    default:
      MOZ_ASSERT(false, "Path not being copied");
      return;
  }
}

}  // namespace gfx
}  // namespace mozilla
