#pragma once

#include "IControl.h"

using namespace iplug;
using namespace igraphics;

// Background artwork (resources/img/background.png -- see CLAUDE.md), stretched to fill the
// current bounds on every resize. Deliberately not IGraphics::AttachBackground()'s plain
// IBitmapControl: that draws the bitmap at native size centred in its rect, which would leave
// gaps (or crop) the moment the host resizes away from the image's native aspect ratio. This
// control instead draws via DrawFittedBitmap, so it rescales non-uniformly with mRECT exactly
// like AttachPanelBackground's flat-color panel already does -- mLayoutFunc's resize branch
// re-targets it the same way, via GetBackgroundControl()->SetTargetAndDrawRECTs(bounds).
//
// The artwork bakes in the plugin's logo/tagline (top-left) and two vertical divider lines that
// mark where the left tab column and right indicator column should sit -- mLayoutFunc's column
// fractions (kLeftColFrac/kRightColFrac) are measured directly off this image so they track it
// exactly across the full resize range, regardless of window aspect ratio.
class BackgroundImageControl : public IControl
{
public:
  BackgroundImageControl(const IRECT& bounds, const IBitmap& bitmap)
  : IControl(bounds)
  , mBitmap(bitmap)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    g.DrawFittedBitmap(mBitmap, mRECT);
  }

private:
  IBitmap mBitmap;
};
