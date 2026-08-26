#pragma once

#include "IControl.h"
#include "ISender.h"

using namespace iplug;
using namespace igraphics;

// led_shine.png (a glow baked around the same bird icon as the badge circle in background.png,
// see CLAUDE.md) that suddenly appears at full opacity, then fades back off over
// kFlashDurationMs, each time it receives a message via ISender<1> -- fed from ProcessBlock at
// the moment the envelope crosses the trigger threshold. Uses IControl's built-in animation timer
// (see SetAnimation) rather than its own clock, since IGraphics already drives that once per
// frame. Was a plain vector-drawn dot (FillEllipse/DrawEllipse) before the badge art existed.
class TriggerLightControl : public IControl
{
public:
  static constexpr int kFlashDurationMs = 150;

  TriggerLightControl(const IRECT& bounds, const IBitmap& bitmap)
  : IControl(bounds)
  , mBitmap(bitmap)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    if (mBrightness <= 0.f)
      return;

    const IBlend blend(EBlend::Default, mBrightness);
    g.DrawFittedBitmap(mBitmap, mRECT, &blend);
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
  IBitmap mBitmap;
};
