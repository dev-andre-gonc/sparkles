#pragma once

#include "IControl.h"
#include "ISender.h"
#include "Palette.h"

using namespace iplug;
using namespace igraphics;

// Small circular light that flashes on, then fades back off over kFlashDurationMs, each time it
// receives a message via ISender<1> -- fed from ProcessBlock at the moment the envelope crosses
// the trigger threshold. Uses IControl's built-in animation timer (see SetAnimation) rather than
// its own clock, since IGraphics already drives that once per frame.
class TriggerLightControl : public IControl
{
public:
  static constexpr int kFlashDurationMs = 150;

  TriggerLightControl(const IRECT& bounds, const IColor& onColor = sparkle_palette::kCustomRed, const IColor& offColor = sparkle_palette::kLinesInterior.WithOpacity(0.3f))
  : IControl(bounds)
  , mOnColor(onColor)
  , mOffColor(offColor)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    const IColor color = IColor::LinearInterpolateBetween(mOffColor, mOnColor, mBrightness);
    g.FillEllipse(color, mRECT);
    g.DrawEllipse(sparkle_palette::kLinesOuter, mRECT);
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    if (msgTag == ISender<>::kUpdateMessage)
    {
      SetAnimation([this](IControl* pCaller) {
        const double progress = pCaller->GetAnimationProgress();

        if (progress >= 1.)
        {
          mBrightness = 0.f;
          pCaller->OnEndAnimation();
          return;
        }

        mBrightness = 1.f - static_cast<float>(progress);
        pCaller->SetDirty(false);
      }, kFlashDurationMs);
    }
  }

private:
  float mBrightness = 0.f;
  IColor mOnColor;
  IColor mOffColor;
};
