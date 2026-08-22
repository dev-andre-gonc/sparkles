#pragma once

#include "IControl.h"
#include "Palette.h"

using namespace iplug;
using namespace igraphics;

// Plain rounded-rect backdrop -- a translucent "frosted glass" card that sits behind a region of
// controls so their text/labels stay legible over Sparkles.cpp's gradient background image
// regardless of which part of the gradient happens to be behind them. Mouse-transparent and drawn
// with no other content, so it's attached once per column right after the background and before
// that column's real controls (draw order follows attach order).
class CardPanelControl : public IControl
{
public:
  CardPanelControl(const IRECT& bounds, const IColor& fillColor = sparkle_palette::kCardFill,
                    const IColor& frameColor = sparkle_palette::kCardFrame, float cornerRadius = 14.f)
  : IControl(bounds)
  , mFillColor(fillColor)
  , mFrameColor(frameColor)
  , mCornerRadius(cornerRadius)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    g.FillRoundRect(mFillColor, mRECT, mCornerRadius);
    g.DrawRoundRect(mFrameColor, mRECT, mCornerRadius, nullptr, 1.5f);
  }

private:
  IColor mFillColor;
  IColor mFrameColor;
  float mCornerRadius;
};
