#pragma once

#include "IControl.h"
#include "ISender.h"
#include "IPlugStructs.h"

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
    g.FillRect(COLOR_BLACK, mRECT);

    const IRECT envelopeRect = mRECT.FracRect(EDirection::Vertical, mEnvelope);
    g.FillRect(COLOR_GREEN, envelopeRect);

    const float thresholdY = mRECT.B - static_cast<float>(GetValue()) * mRECT.H();
    g.DrawLine(COLOR_RED, mRECT.L, thresholdY, mRECT.R, thresholdY, nullptr, 2.f);

    g.DrawRect(COLOR_WHITE, mRECT);
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
