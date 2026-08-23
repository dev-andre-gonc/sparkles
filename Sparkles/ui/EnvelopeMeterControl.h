#pragma once

#include "IControl.h"
#include "ISender.h"
#include "IPlugStructs.h"
#include "Palette.h"

#include <algorithm>

using namespace iplug;
using namespace igraphics;

// Vertical meter showing the envelope-follower level (pushed once per block via ISender<1> from
// ProcessBlock) as a filled bar, with a horizontal line marking the current amplitude threshold.
// The threshold line is driven by binding this control to the threshold param directly -- IGraphics
// keeps any control bound to a paramIdx in sync automatically, the same way the existing threshold
// slider is -- so no separate message path is needed for it. The control itself is read-only: mouse
// input is ignored so dragging it can't also drag the threshold slider it shares a param with.
class EnvelopeMeterControl : public IControl
{
public:
  EnvelopeMeterControl(const IRECT& bounds, int thresholdParamIdx)
  : IControl(bounds, thresholdParamIdx)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    using namespace sparkle_palette;
    const float cr = mRECT.W() * 0.25f;
    // Square bottom corners (0,0), rounded top (cr,cr) -- matches the envelope fill below, which
    // is square-bottomed too (it sits flush on this container's bottom edge). Rounding all four
    // corners here while the fill only rounds its top left a visible mismatch/gap at the bottom
    // two corners whenever the bar was tall enough to reach them.
    g.FillRoundRect(kLinesOuter.WithOpacity(0.15f), mRECT, cr, cr, 0.f, 0.f);

    // The fill's bottom edge always sits flush on the meter's own bottom edge, so rounding its
    // bottom corners too just doubled up the curve and looked like a notch -- only the rising top
    // edge should be rounded, like a liquid level. The top radius is still capped to the fill's
    // own (short) height so it can't overshoot a barely-there bar.
    const IRECT envelopeRect = mRECT.FracRect(EDirection::Vertical, mEnvelope);
    const float topR = std::min(cr, envelopeRect.H() * 0.5f);
    g.FillRoundRect(kPearlFrost.WithOpacity(0.85f), envelopeRect, topR, topR, 0.f, 0.f);

    const float thresholdY = mRECT.B - static_cast<float>(GetValue()) * mRECT.H();
    g.DrawLine(kCustomRed, mRECT.L, thresholdY, mRECT.R, thresholdY, nullptr, 2.f);

    g.DrawRoundRect(kLinesOuter, mRECT, cr, cr, 0.f, 0.f, nullptr, kLineThickness);
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    if (msgTag == ISender<>::kUpdateMessage)
    {
      IByteStream stream(pData, dataSize);
      ISenderData<1> d;
      stream.Get(&d, 0);
      mEnvelope = Clip(d.vals[0], 0.f, 1.f);
      SetDirty(false);
    }
  }

private:
  float mEnvelope = 0.f;
};
