#pragma once

#include "IControl.h"
#include "ISender.h"
#include "IPlugStructs.h"
#include <array>
#include <functional>

using namespace iplug;
using namespace igraphics;

// Text control that displays N float values pushed from the audio thread via ISender<N>,
// formatted by a caller-supplied function -- shared between the detected-note display (N=2:
// formats a MIDI note number + confidence as e.g. "C4 87%") and the active-sprinkle-count
// display (N=1: formats an int as text).
template <int N = 1>
class ValueDisplayControl : public ITextControl
{
public:
  using FormatFunc = std::function<void(const std::array<float, N>& vals, WDL_String& str)>;

  ValueDisplayControl(const IRECT& bounds, const char* defaultStr, const IText& text, FormatFunc formatFunc)
  : ITextControl(bounds, defaultStr, text)
  , mFormatFunc(std::move(formatFunc))
  {
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    if (msgTag == ISender<>::kUpdateMessage)
    {
      IByteStream stream(pData, dataSize);
      ISenderData<N> d;
      stream.Get(&d, 0);

      WDL_String str;
      mFormatFunc(d.vals, str);
      SetStr(str.Get());
      SetDirty(false);
    }
  }

private:
  FormatFunc mFormatFunc;
};
