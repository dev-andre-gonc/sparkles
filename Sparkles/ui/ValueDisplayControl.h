#pragma once

#include "IControl.h"
#include "ISender.h"
#include "IPlugStructs.h"
#include <functional>

using namespace iplug;
using namespace igraphics;

// Text control that displays a single float value pushed from the audio thread via ISender<1>,
// formatted by a caller-supplied function -- shared between the detected-note display (formats a
// MIDI note number as e.g. "C4") and the active-sprinkle-count display (formats an int as text).
class ValueDisplayControl : public ITextControl
{
public:
  using FormatFunc = std::function<void(float value, WDL_String& str)>;

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
      ISenderData<1> d;
      stream.Get(&d, 0);

      WDL_String str;
      mFormatFunc(d.vals[0], str);
      SetStr(str.Get());
      SetDirty(false);
    }
  }

private:
  FormatFunc mFormatFunc;
};
