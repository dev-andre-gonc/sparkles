#pragma once

#include "IControl.h"
#include "Palette.h"

using namespace iplug;
using namespace igraphics;

// Small dot-and-line marker drawn to the left of a param group's heading (see Sparkles.cpp's
// kGroupMarkerR/kGroupMarkerTextGap and the groupMarkerBounds loop in mLayoutFunc) -- a lighter
// stand-in for the old bordered box around each group's knobs. Purely decorative, so bounds are
// reused as the geometry itself rather than a widget rect: mRECT.W() sets the dot's diameter (the
// dot is drawn inscribed in the top of mRECT), and the line runs from a few px below the dot down
// to mRECT.B, which the layout code sets to the group's own lowest cluster.
class GroupMarkerControl : public IControl
{
public:
  GroupMarkerControl(const IRECT& bounds, const IColor& color = sparkle_palette::kLinesOuter)
  : IControl(bounds)
  , mColor(color.WithOpacity(kOpacity))
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    const float r = mRECT.W() * 0.5f;
    const float cx = mRECT.MW();
    const float cy = mRECT.T + r;
    g.FillEllipse(mColor, IRECT(cx - r, cy - r, cx + r, cy + r));

    const float lineTop = cy + r + kLineGapBelowDot;
    if (mRECT.B > lineTop)
      g.DrawLine(mColor, cx, lineTop, cx, mRECT.B, nullptr, sparkle_palette::kLineThickness);
  }

private:
  static constexpr float kLineGapBelowDot = 3.f;
  static constexpr float kOpacity = 0.3f;
  IColor mColor;
};
